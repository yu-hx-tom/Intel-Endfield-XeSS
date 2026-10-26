"""Bounded PE x64 string / RIP-relative reference inspection.

Reads PE headers, .rdata, .pdata and .text (in 1 MiB chunks). It does not execute
or modify the input. Raw byte matches are candidates; only instruction addresses
decoded by llvm-objdump from a .pdata/chained-unwind boundary are marked verified.
By default only LEA references to four named strings are scanned. Explicit extra
RVA targets enable selected MOV/MOVZX/CMP/indirect CALL/JMP and E8/E9 searches.
"""
import argparse
import bisect
import datetime
import json
import pathlib
import re
import struct
import subprocess

STRINGS = (
    "Initializing Streamline",
    "disable-streamline-at-startup",
    "slSetTagForFrame",
    "slHGSetupCustomVulkan",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=pathlib.Path, required=True)
    parser.add_argument("--objdump", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--rip-rva", action="append", default=[], help="Additional named RIP target LABEL:RVA")
    parser.add_argument("--call-rva", action="append", default=[], help="Named direct E8/E9 branch target LABEL:RVA")
    parser.add_argument("--max-references-per-target", type=int, default=16)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    reads = []
    with args.input.open("rb") as source:
        def read(offset, size, reason):
            if size < 0 or size > 8 * 1024 * 1024:
                raise ValueError("Read exceeded bounded inspection size")
            source.seek(offset)
            value = source.read(size)
            if len(value) != size:
                raise ValueError("Unexpected end of PE file")
            reads.append({"offset": offset, "size": size, "reason": reason})
            return value

        dos = read(0, 64, "DOS header")
        if dos[:2] != b"MZ":
            raise ValueError("Not PE")
        nt_offset = struct.unpack_from("<I", dos, 60)[0]
        nt = read(nt_offset, 24, "NT/COFF header")
        if nt[:4] != b"PE\0\0":
            raise ValueError("Invalid PE signature")
        machine, section_count = struct.unpack_from("<HH", nt, 4)
        optional_size = struct.unpack_from("<H", nt, 20)[0]
        if machine != 0x8664 or section_count > 64:
            raise ValueError("Only bounded x64 PE supported")
        optional = read(nt_offset + 24, optional_size, "Optional header")
        if struct.unpack_from("<H", optional)[0] != 0x20b:
            raise ValueError("Not PE32+")
        image_base = struct.unpack_from("<Q", optional, 24)[0]
        section_bytes = read(nt_offset + 24 + optional_size, section_count * 40, "Section table")
        sections = {}
        for index in range(section_count):
            record = section_bytes[index * 40:(index + 1) * 40]
            name = record[:8].rstrip(b"\0").decode("ascii")
            virtual_size, rva, raw_size, offset = struct.unpack_from("<IIII", record, 8)
            sections[name] = dict(virtual_size=virtual_size, rva=rva, raw_size=raw_size, offset=offset)

        rdata = sections[".rdata"]
        string_bytes = read(rdata["offset"], rdata["raw_size"], "Targeted ASCII search in .rdata")
        targets = {}
        strings = []
        for name in STRINGS:
            needle = name.encode("ascii")
            offset = 0
            matches = []
            while True:
                position = string_bytes.find(needle, offset)
                if position < 0:
                    break
                rva = rdata["rva"] + position
                targets[rva] = name
                matches.append({"rva": hex(rva), "va": hex(image_base + rva), "file_offset": rdata["offset"] + position})
                offset = position + 1
            strings.append({"string": name, "matches": matches})
        del string_bytes
        for value in args.rip_rva:
            name, value = value.rsplit(":", 1)
            targets[int(value, 0)] = name
        call_targets = {}
        for value in args.call_rva:
            name, value = value.rsplit(":", 1)
            call_targets[int(value, 0)] = name

        pdata = sections[".pdata"]
        pdata_bytes = read(pdata["offset"], pdata["raw_size"], "Runtime function boundaries")
        functions = [struct.unpack_from("<III", pdata_bytes, i) for i in range(0, len(pdata_bytes) - 11, 12)]
        functions = sorted((begin, end, unwind) for begin, end, unwind in functions if begin and end > begin)
        starts = [entry[0] for entry in functions]

        def rva_offset(rva):
            for section in sections.values():
                if section["rva"] <= rva < section["rva"] + section["raw_size"]:
                    return section["offset"] + rva - section["rva"]
            raise ValueError("Unwind RVA is not file-backed")

        def original_function(region):
            chain = [region]
            for _ in range(8):
                unwind = chain[-1][2]
                header = read(rva_offset(unwind), 4, "Selected unwind header")
                flags = header[0] >> 3
                if not flags & 4:  # UNW_FLAG_CHAININFO
                    return chain[-1][0], chain
                # UNWIND_CODE array is aligned to a DWORD before chained entry.
                chained_rva = unwind + 4 + ((header[2] + 1) & ~1) * 2
                previous = struct.unpack("<III", read(rva_offset(chained_rva), 12, "Selected chained runtime function"))
                if previous in chain:
                    raise ValueError("Unwind chain loop")
                chain.append(previous)
            raise ValueError("Unwind chain exceeds bounded depth")
        candidates = []
        target_match_counts = {}
        text = sections[".text"]
        chunk_size = 1024 * 1024
        for start in range(0, text["raw_size"], chunk_size):
            length = min(chunk_size + 8, text["raw_size"] - start)
            chunk = read(text["offset"] + start, length, "RIP-relative LEA candidates in .text")
            pattern = b"[\x8b\x8d\x80\xe8\xe9\xff]|\x0f[\xb6\xb7]" if args.rip_rva or call_targets else b"\x8d"
            for match in re.finditer(pattern, chunk):
                opcode = match.start()
                is_call = chunk[opcode] in (0xe8, 0xe9)
                two_byte = chunk[opcode] == 0x0f
                tail_immediate = 1 if chunk[opcode] == 0x80 else 0
                end = opcode + (5 if is_call else 6 + int(two_byte) + tail_immediate)
                if end > len(chunk):
                    continue
                modrm = chunk[opcode + 1 + int(two_byte)]
                if not is_call and modrm & 0xc7 != 0x05:
                    continue
                instruction = opcode
                if not is_call and opcode > 0 and 0x40 <= chunk[opcode - 1] <= 0x4f:
                    instruction -= 1
                if instruction >= chunk_size:
                    continue
                displacement = struct.unpack_from("<i", chunk, opcode + (1 if is_call else 2 + int(two_byte)))[0]
                target = text["rva"] + start + end + displacement
                lookup = call_targets if is_call else targets
                if target not in lookup:
                    continue
                target_match_counts[target] = target_match_counts.get(target, 0) + 1
                if target_match_counts[target] > args.max_references_per_target:
                    continue
                rva = text["rva"] + start + instruction
                function_index = bisect.bisect_right(starts, rva) - 1
                function = functions[function_index] if function_index >= 0 else None
                if function and rva >= function[1]:
                    function = None
                original, chain = original_function(function) if function else (None, [])
                candidates.append({"string": lookup[target], "reference_kind": "call" if is_call else "rip", "target_rva": hex(target), "target_va": hex(image_base + target),
                                   "instruction_rva": hex(rva), "instruction_va": hex(image_base + rva),
                                   "candidate_bytes": chunk[instruction:end].hex(" "),
                                   "runtime_region_rva": hex(function[0]) if function else None,
                                   "unwind_chain": [[hex(value) for value in entry] for entry in chain],
                                   "function_rva": hex(original) if function else None,
                                   "function_va": hex(image_base + original) if function else None,
                                   "function_end_rva": hex(function[1]) if function else None,
                                   "verified_instruction": False})

    disassemblies = {}
    for candidate in candidates:
        if not candidate["function_rva"]:
            candidate["verification_note"] = "No containing .pdata function"
            continue
        begin = int(candidate["function_rva"], 16)
        end = int(candidate["function_end_rva"], 16)
        instruction = int(candidate["instruction_rva"], 16)
        if instruction - begin > 16 * 1024:
            candidate["verification_note"] = "Beyond 16 KiB per-function decode budget"
            continue
        if begin not in disassemblies:
            stop = min(end, begin + 16 * 1024)
            command = [str(args.objdump), "-d", "--x86-asm-syntax=intel", f"--start-address={image_base + begin}",
                       f"--stop-address={image_base + stop}", str(args.input)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            if result.returncode:
                raise RuntimeError(result.stderr)
            path = args.output / f"function-{image_base + begin:x}.asm.txt"
            path.write_text(result.stdout, encoding="utf-8")
            disassemblies[begin] = (result.stdout, str(path))
        assembly, path = disassemblies[begin]
        tail = r":\s+.*\b(?:call|jmp)\b.*$" if candidate["reference_kind"] == "call" else r":\s+.*\b(?:lea|mov|movzx|cmp|call|jmp)\b.*\brip\b.*$"
        pattern = re.compile(r"^\s*" + f"{image_base + instruction:x}" + tail, re.MULTILINE | re.IGNORECASE)
        verified = pattern.search(assembly)
        candidate["disassembly"] = path
        candidate["verified_instruction"] = verified is not None
        candidate["decoded_instruction"] = verified.group(0).strip() if verified else None
    report = {"input": str(args.input), "input_size": args.input.stat().st_size,
              "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "image_base": hex(image_base), "sections": sections, "strings": strings,
              "target_match_counts": {hex(key): value for key, value in target_match_counts.items()},
              "max_references_per_target": args.max_references_per_target,
              "candidates": candidates, "read_operations": reads,
              "limitations": "Selected RIP-relative instructions/direct calls only. Instruction validation begins at .pdata/chained-unwind boundary. No execution or game resource reads."}
    (args.output / "references.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(args.output), "candidates": len(candidates),
                      "verified": sum(item["verified_instruction"] for item in candidates)}))


if __name__ == "__main__":
    main()
