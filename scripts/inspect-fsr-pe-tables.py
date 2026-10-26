"""Read only selected PE .rdata string/table entries; no execution or mutation."""
import json
import hashlib
import pathlib
import re
import struct

target = pathlib.Path(r"D:\games\Hypergryph Launcher\games\Arknights Endfield\UnityPlayer.dll")
out = pathlib.Path(__file__).resolve().parents[1] / "out/2026-10-13T04-53-17-295Z-rea-mcp"
with target.open("rb") as stream:
    digest = hashlib.sha256()
    while chunk := stream.read(65536):
        digest.update(chunk)
    if digest.hexdigest() != "bee7be52370adddd67ba61e4937ca51b7f272656841d187e95e505496da798d1":
        raise ValueError("UnityPlayer version differs from the bounded assembly evidence")
    stream.seek(0x3c)
    pe = struct.unpack("<I", stream.read(4))[0]
    stream.seek(pe)
    header = stream.read(24)
    count = struct.unpack_from("<H", header, 6)[0]
    opt_size = struct.unpack_from("<H", header, 20)[0]
    opt = stream.read(opt_size)
    base = struct.unpack_from("<Q", opt, 24)[0]
    sections = []
    for _ in range(count):
        row = stream.read(40)
        size, rva, raw, offset = struct.unpack_from("<IIII", row, 8)
        sections.append((row[:8].rstrip(b"\0").decode(), rva, size, raw, offset))
    rdata = next(row for row in sections if row[0] == ".rdata")
    assert rdata[3] < 8 * 1024 * 1024
    stream.seek(rdata[4])
    data = stream.read(rdata[3])
    rows = []
    for needle in (b"UnityEngine.HyperGryphEngineCode.HGFSR3Util::IsFSR3Supported", b"UnityEngine.HyperGryphEngineCode.HGFSR3Util::IsFSR3FISupported", b"vkCmdCopyImage", b"UnityEngine.Texture::GetNativeTexturePtr", b"UnityEngine.Texture::GetNativeTextureID"):
        start = data.find(needle + b"\0")
        if start < 0:
            rows.append({"string":needle.decode(),"found":False})
            continue
        address = base + rdata[1] + start
        ptr = struct.pack("<Q", address)
        position = 0
        refs = []
        while True:
            pos = data.find(ptr, position)
            if pos < 0:
                break
            lo = max(0, pos - 32)
            hi = min(len(data), pos + 40)
            refs.append({"va": hex(base+rdata[1]+pos), "nearby_qwords": [{"va":hex(base+rdata[1]+p),"value":hex(struct.unpack_from('<Q',data,p)[0])} for p in range(lo,hi-7,8)]})
            position = pos + 1
        rows.append({"string":needle.decode(),"va":hex(address),"pointer_references":refs})
    strings=[]
    for start in range(0x19c35e0, 0x19c3810):
        pos=start-rdata[4]
        if pos >= 0 and (pos == 0 or data[pos-1] == 0) and 32 <= data[pos] < 127:
            end=data.find(b"\0",pos)
            value=data[pos:end]
            if len(value)>=3 and all(32<=c<127 for c in value):
                strings.append({"va":hex(base+rdata[1]+pos),"value":value.decode()})
    mappings=[]
    assembly=(out/"property-constructor.asm.txt").read_text(encoding="utf-8")
    lines=assembly.splitlines()
    for i,line in enumerate(lines[:-2]):
        offset=re.search(r"lea\s+rcx, \[rbx \+ (0x[0-9a-f]+)\]",line)
        ptr=re.search(r"lea\s+rdx,.*# (0x[0-9a-f]+)",lines[i+1])
        if not offset or not ptr or 'call\t0x1800d1860' not in lines[i+2]:
            continue
        ofs=int(offset[1],16)
        if not (0x13d0<=ofs<=0x15c0 or ofs==0x188c):
            continue
        address=int(ptr[1],16)
        pos=address-base-rdata[1]
        if not 0<=pos<len(data):
            continue
        end=data.find(b'\0',pos)
        value=data[pos:end].decode('ascii')
        mappings.append({"id_address":hex(0x181d0f230+ofs),"name":value,"name_va":hex(address),"constructor_instruction":line.split(':')[0]})
    report={"target":str(target),"sha256":digest.hexdigest(),"image_base":hex(base),"table_candidates":rows,"nearby_property_names":strings,"selected_shader_property_mappings":mappings,"limitation":"Qword byte matches are table candidates, not code xrefs. Shader property mapping assumes constructor base 0x181d0f230, confirmed by direct call at 0x1803f608a."}
    (out/"fsr-table-candidates.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(json.dumps(report,indent=2))
