"""Find direct x64 CALL references to an RVA in a PE image.

Usage:
    python -I Tools/FindCallXrefs.py <image.exe> <target-rva>

The reconstructed Minecraft image emitted by Oinkster preserves section RVAs,
so output can be passed directly to dumpbin /disasm /range.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from FindStringXrefs import parse_sections


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("target_rva", type=lambda value: int(value, 0))
    args = parser.parse_args()

    data = args.image.read_bytes()
    matches: list[tuple[int, bytes]] = []
    for section in parse_sections(data):
        if section.characteristics & 0x20000000 == 0:
            continue
        code = data[section.raw_offset : section.raw_offset + section.raw_size]
        for index in range(max(0, len(code) - 5)):
            if code[index] != 0xE8:
                continue
            displacement = struct.unpack_from("<i", code, index + 1)[0]
            call_rva = section.virtual_address + index
            if call_rva + 5 + displacement != args.target_rva:
                continue
            start = max(0, index - 24)
            end = min(len(code), index + 29)
            matches.append((call_rva, bytes(code[start:end])))

    print(f"target rva={args.target_rva:#x} direct-call-xrefs={len(matches)}")
    for call_rva, context in matches:
        print(f"  call rva={call_rva:#x} context={context.hex(' ')}")


if __name__ == "__main__":
    main()
