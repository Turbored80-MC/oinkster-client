import struct
import sys
from pathlib import Path


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


path = Path(sys.argv[1])
data = path.read_bytes()
pe = u32(data, 0x3C)
coff = pe + 4
section_count = u16(data, coff + 2)
optional_size = u16(data, coff + 16)
optional = coff + 20
image_base = u64(data, optional + 24)
section_table = optional + optional_size
sections = []
for index in range(section_count):
    entry = section_table + index * 40
    name = data[entry:entry + 8].split(b"\0", 1)[0].decode("ascii", "replace")
    virtual_size = u32(data, entry + 8)
    virtual_address = u32(data, entry + 12)
    raw_size = u32(data, entry + 16)
    raw_offset = u32(data, entry + 20)
    sections.append((name, virtual_address, virtual_size, raw_offset, raw_size))


def rva_to_offset(rva):
    for _, virtual_address, virtual_size, raw_offset, raw_size in sections:
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_offset + rva - virtual_address
    return rva


import_directory = optional + 112 + 8
import_rva = u32(data, import_directory)
descriptor = rva_to_offset(import_rva)
targets = {}
while True:
    original_thunk, _, _, name_rva, first_thunk = struct.unpack_from("<IIIII", data, descriptor)
    if not any((original_thunk, name_rva, first_thunk)):
        break
    name_offset = rva_to_offset(name_rva)
    end = data.index(0, name_offset)
    dll_name = data[name_offset:end].decode("ascii", "replace")
    thunk_rva = original_thunk or first_thunk
    thunk_offset = rva_to_offset(thunk_rva)
    index = 0
    while True:
        thunk = u64(data, thunk_offset + index * 8)
        if thunk == 0:
            break
        if (thunk >> 63) == 0:
            import_name_offset = rva_to_offset(thunk) + 2
            import_name_end = data.index(0, import_name_offset)
            import_name = data[import_name_offset:import_name_end].decode("ascii", "replace")
            if import_name in {"GetRawInputData", "RegisterRawInputDevices", "PeekMessageW", "DispatchMessageW"}:
                targets[import_name] = image_base + first_thunk + index * 8
                print(f"{dll_name}!{import_name} IAT={targets[import_name]:#x}")
        index += 1
    descriptor += 20

text = next(section for section in sections if section[0] == ".text")
_, text_rva, _, text_offset, text_size = text
code = memoryview(data)[text_offset:text_offset + text_size]
for name, target in targets.items():
    hits = []
    for index in range(len(code) - 6):
        if code[index] == 0xFF and code[index + 1] in (0x15, 0x25):
            displacement = struct.unpack_from("<i", code, index + 2)[0]
            instruction_va = image_base + text_rva + index
            resolved = instruction_va + 6 + displacement
            if resolved == target:
                hits.append(instruction_va)
    print(f"{name} xrefs={len(hits)} " + " ".join(f"{hit:#x}" for hit in hits))
