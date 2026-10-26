"""Read bounded PE data sections for four exact FSR callback addresses.

This is a static file inspection. It neither opens a game process nor executes
the inspected binary. Absolute-pointer matches are candidates, not callable ABI
proof; absence says nothing about dynamically initialized heap tables.
"""
import hashlib
import json
import pathlib
import struct

root = pathlib.Path(__file__).resolve().parents[1]
source = pathlib.Path(r"D:\games\Hypergryph Launcher\games\Arknights Endfield\UnityPlayer.dll")
output = root / "out/fsr-callback-static-20261013/static-pointer-candidates.json"
expected = "bee7be52370adddd67ba61e4937ca51b7f272656841d187e95e505496da798d1"
targets = {
    "shared_executor_thunk": 0x180E7A6A0,
    "shared_executor": 0x180E7C040,
    "fi_prepare_registration": 0x180E7B4D0,
    "other_fi_registration": 0x180E7AF20,
}
with source.open("rb") as stream:
    digest = hashlib.sha256()
    while chunk := stream.read(65536):
        digest.update(chunk)
    if digest.hexdigest() != expected:
        raise ValueError("Inspected binary does not match the existing analysis")
    stream.seek(0x3C)
    pe = struct.unpack("<I", stream.read(4))[0]
    stream.seek(pe)
    header = stream.read(24)
    if header[:4] != b"PE\0\0":
        raise ValueError("Not a PE file")
    count = struct.unpack_from("<H", header, 6)[0]
    opt_size = struct.unpack_from("<H", header, 20)[0]
    if count > 64 or opt_size > 4096:
        raise ValueError("Unexpected PE header dimensions")
    optional = stream.read(opt_size)
    base = struct.unpack_from("<Q", optional, 24)[0]
    sections = []
    for _ in range(count):
        raw = stream.read(40)
        virtual_size, rva, size, offset = struct.unpack_from("<IIII", raw, 8)
        sections.append((raw[:8].rstrip(b"\0").decode(), rva, size, offset))
    inspected, matches = [], []
    for name, rva, size, offset in sections:
        if name not in (".rdata", ".data", ".fptable", "_RDATA"):
            continue
        if size > 8 * 1024 * 1024:
            raise ValueError("Section exceeds bounded read limit")
        stream.seek(offset)
        data = stream.read(size)
        inspected.append({"section": name, "offset": offset, "bytes": size})
        for label, address in targets.items():
            needle = struct.pack("<Q", address)
            position = 0
            while (position := data.find(needle, position)) != -1:
                matches.append({"target": label, "target_va": hex(address),
                                "section": name, "candidate_va": hex(base + rva + position),
                                "aligned_8": (rva + position) % 8 == 0})
                position += 1
    stream.seek(0x19C371C)
    kernel_names = stream.read(64).split(b"\0")
report = {"input": str(source), "sha256": digest.hexdigest(), "image_base": hex(base),
          "targets": {key: hex(value) for key, value in targets.items()},
          "inspected_sections": inspected, "absolute_pointer_candidates": matches,
          "kernel_name_va": "0x1819c4f1c", "kernel_name": kernel_names[0].decode("ascii"),
          "limitation": "Only exact preferred-base qwords in selected file-backed PE data sections; no dynamic heap/global population conclusion."}
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(report, indent=2), encoding="utf-8")
print(json.dumps(report, indent=2))
