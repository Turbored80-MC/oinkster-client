"""Find x64 RIP-relative references to a string in a PE image.

The input may be either the installed image or the reconstructed image emitted
by Oinkster's debug diagnostics.  Output uses RVAs so results are independent
of ASLR and can be compared with a disassembler.

Usage:
    python -I Tools/FindStringXrefs.py <image.exe> <ascii-string>
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Section:
    name: str
    virtual_address: int
    virtual_size: int
    raw_offset: int
    raw_size: int
    characteristics: int


def read_u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def read_u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_sections(data: bytes) -> list[Section]:
    pe = read_u32(data, 0x3C)
    if data[pe : pe + 4] != b"PE\0\0":
        raise ValueError("input is not a PE image")
    coff = pe + 4
    section_count = read_u16(data, coff + 2)
    optional_size = read_u16(data, coff + 16)
    section_table = coff + 20 + optional_size
    sections = []
    for index in range(section_count):
        entry = section_table + index * 40
        name = data[entry : entry + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        virtual_size = read_u32(data, entry + 8)
        virtual_address = read_u32(data, entry + 12)
        raw_size = read_u32(data, entry + 16)
        raw_offset = read_u32(data, entry + 20)
        characteristics = read_u32(data, entry + 36)
        sections.append(Section(name, virtual_address, virtual_size, raw_offset, raw_size, characteristics))
    return sections


def offset_to_rva(sections: list[Section], offset: int) -> int | None:
    for section in sections:
        if section.raw_offset <= offset < section.raw_offset + section.raw_size:
            return section.virtual_address + offset - section.raw_offset
    return None


def rva_to_offset(sections: list[Section], rva: int) -> int | None:
    for section in sections:
        span = max(section.virtual_size, section.raw_size)
        if section.virtual_address <= rva < section.virtual_address + span:
            return section.raw_offset + rva - section.virtual_address
    return None


def find_string_rvas(data: bytes, sections: list[Section], needle: bytes) -> list[int]:
    results = []
    cursor = 0
    while True:
        cursor = data.find(needle, cursor)
        if cursor < 0:
            return results
        rva = offset_to_rva(sections, cursor)
        if rva is not None:
            results.append(rva)
        cursor += 1


def find_rip_references(data: bytes, sections: list[Section], target_rva: int) -> list[tuple[int, bytes]]:
    results = []
    for section in sections:
        if section.characteristics & 0x20000000 == 0:  # IMAGE_SCN_MEM_EXECUTE
            continue
        code = data[section.raw_offset : section.raw_offset + section.raw_size]
        # The relevant clang/MSVC forms are REX + LEA/MOV reg,[RIP+disp32].
        # Check every byte rather than trying to disassemble untrusted input.
        for index in range(max(0, len(code) - 7)):
            rex = code[index]
            opcode = code[index + 1]
            modrm = code[index + 2]
            if rex & 0xF0 != 0x40 or opcode not in (0x8D, 0x8B) or modrm & 0xC7 != 0x05:
                continue
            displacement = struct.unpack_from("<i", code, index + 3)[0]
            instruction_rva = section.virtual_address + index
            if instruction_rva + 7 + displacement == target_rva:
                start = max(0, index - 16)
                end = min(len(code), index + 23)
                results.append((instruction_rva, bytes(code[start:end])))
    return results


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("text")
    args = parser.parse_args()

    data = args.image.read_bytes()
    sections = parse_sections(data)
    needles = [("ascii", args.text.encode("utf-8") + b"\0"), ("utf16", args.text.encode("utf-16le") + b"\0\0")]
    found = False
    for encoding, needle in needles:
        for string_rva in find_string_rvas(data, sections, needle):
            found = True
            print(f"{encoding} string rva={string_rva:#x}")
            references = find_rip_references(data, sections, string_rva)
            if not references:
                print("  no direct RIP-relative references")
            for reference_rva, context in references:
                print(f"  xref rva={reference_rva:#x} context={context.hex(' ')}")
    if not found:
        raise SystemExit("string was not found")


if __name__ == "__main__":
    main()
