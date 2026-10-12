"""Survey byte signatures against a Minecraft.Windows image's .text section.

Usage:
    python -I Tools/SurveySignatures.py <image.exe> [--flarial <SigInit.cpp>] [--pattern NAME=HEX ...] [--only SUBSTR]
    python -I Tools/SurveySignatures.py --pid <minecraft-pid> [same options]

Resolves Flarial dll-oss's cumulative signature table (every version's init
applied oldest -> newest, honouring DEPRECATE_SIG) and reports, for each
pattern, whether it is unique / ambiguous / missing in .text - the same rule
SignatureScan::FindUniqueInText enforces at runtime. Read-only.
"""
import argparse
import ctypes
import re
import struct
import sys
from pathlib import Path

# Oldest -> newest, mirroring Flarial's VersionUtils::initialize (reversed).
FLARIAL_ORDER = [
    "2030", "2040", "2050", "2060", "2070", "2080", "2100", "2102", "2120", "2130",
    "2140", "2150", "2160", "2170", "2180", "2190", "21100", "21110", "21120",
    "21130", "21131", "260",
]

# This repo's own runtime patterns, so a survey also covers what we ship.
OINKSTER_PATTERNS = {
    "oinkster:InGamePlayScreen::_renderLevelPrep":
        "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? "
        "44 0F 29 9D ? ? ? ? 44 0F 29 95 ? ? ? ? 44 0F 29 8D ? ? ? ? 44 0F 29 85 ? ? ? ? "
        "0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF "
        "4D 89 CC 4D 89 C7 49 89 D6 48 8D 81 ? ? ? ? 48 89 85 ? ? ? ? 48 89 8D ? ? ? ? 48 83 B9 ? ? ? ? 00",
    "oinkster:ScreenView::setupAndRender#push": "48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? "
        "48 81 EC ? ? ? ? 0F 29 BC 24 ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 4C 8B FA",
    "oinkster:ScreenView::setupAndRender#movrax": "48 8B C4 48 89 58 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? "
        "48 81 EC ? ? ? ? 0F 29 70 ? 0F 29 78 ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 4C 8B FA 48 89 55",
}


def load_text(path):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    coff = pe + 4
    count = struct.unpack_from("<H", data, coff + 2)[0]
    optional_size = struct.unpack_from("<H", data, coff + 16)[0]
    table = coff + 20 + optional_size
    for index in range(count):
        entry = table + index * 40
        name = data[entry:entry + 8].split(b"\0", 1)[0]
        if name == b".text":
            virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, entry + 8)
            size = min(max(virtual_size, raw_size), len(data) - raw_offset)
            return data[raw_offset:raw_offset + size], virtual_address
    raise SystemExit("no .text section")


def load_live_text(pid):
    """Read only the mapped PE headers and .text section from a live process."""
    if sys.platform != "win32":
        raise SystemExit("--pid is supported only on Windows")

    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.ReadProcessMemory.argtypes = (
        wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
    kernel32.ReadProcessMemory.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    psapi.EnumProcessModulesEx.argtypes = (
        wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD), wintypes.DWORD)
    psapi.EnumProcessModulesEx.restype = wintypes.BOOL
    psapi.GetModuleBaseNameW.argtypes = (
        wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD)
    psapi.GetModuleBaseNameW.restype = wintypes.DWORD
    process = kernel32.OpenProcess(0x0400 | 0x0010, False, pid)  # QUERY_INFORMATION | VM_READ
    if not process:
        raise SystemExit(f"OpenProcess failed: {ctypes.get_last_error()}")

    try:
        modules = (wintypes.HMODULE * 2048)()
        needed = wintypes.DWORD()
        if not psapi.EnumProcessModulesEx(process, modules, ctypes.sizeof(modules), ctypes.byref(needed), 0x03):
            raise SystemExit(f"EnumProcessModulesEx failed: {ctypes.get_last_error()}")
        count = min(needed.value // ctypes.sizeof(wintypes.HMODULE), len(modules))
        module = None
        module_name = ctypes.create_unicode_buffer(32768)
        for candidate in modules[:count]:
            if not psapi.GetModuleBaseNameW(process, candidate, module_name, len(module_name)):
                continue
            if module_name.value.lower() == "minecraft.windows.exe":
                module = candidate
                break
        if module is None:
            raise SystemExit("Minecraft.Windows.exe was not found in the target process")
        base = ctypes.cast(module, ctypes.c_void_p).value

        def read(address, size):
            buffer = ctypes.create_string_buffer(size)
            received = ctypes.c_size_t()
            if not kernel32.ReadProcessMemory(process, ctypes.c_void_p(address), buffer, size, ctypes.byref(received)):
                raise SystemExit(f"ReadProcessMemory({address:#x}, {size:#x}) failed: {ctypes.get_last_error()}")
            if received.value != size:
                raise SystemExit(f"short process read at {address:#x}: {received.value:#x}/{size:#x}")
            return buffer.raw

        headers = read(base, 0x1000)
        pe = struct.unpack_from("<I", headers, 0x3C)[0]
        if pe + 24 > len(headers):
            headers = read(base, 0x4000)
        if headers[pe:pe + 4] != b"PE\0\0":
            raise SystemExit("the live main module is not a PE image")
        coff = pe + 4
        count = struct.unpack_from("<H", headers, coff + 2)[0]
        timestamp = struct.unpack_from("<I", headers, coff + 4)[0]
        optional_size = struct.unpack_from("<H", headers, coff + 16)[0]
        table = coff + 20 + optional_size
        required = table + count * 40
        if required > len(headers):
            headers = read(base, (required + 0xFFF) & ~0xFFF)
        for index in range(count):
            entry = table + index * 40
            name = headers[entry:entry + 8].split(b"\0", 1)[0]
            if name == b".text":
                virtual_size, virtual_address = struct.unpack_from("<II", headers, entry + 8)
                print(f"live-image  pid={pid} base={base:#x} peTimestamp={timestamp:#x} textRva={virtual_address:#x} textSize={virtual_size:#x}")
                return read(base + virtual_address, virtual_size), virtual_address
        raise SystemExit("no live .text section")
    finally:
        kernel32.CloseHandle(process)


def compile_pattern(text):
    parts = []
    for token in text.split():
        if token.strip("?") == "":
            parts.append(b".")
        else:
            parts.append(re.escape(bytes([int(token, 16)])))
    return re.compile(b"".join(parts), re.DOTALL)


def flarial_table(source):
    text = source.read_text(encoding="utf-8", errors="replace")
    bodies = {}
    for match in re.finditer(r"void\s+SigInit::init(\w+)\s*\(\s*\)", text):
        start = text.index("{", match.end())
        depth, cursor = 0, start
        while True:
            if text[cursor] == "{":
                depth += 1
            elif text[cursor] == "}":
                depth -= 1
                if depth == 0:
                    break
            cursor += 1
        # Drop line comments so commented-out sigs are not applied.
        body = re.sub(r"//[^\n]*", "", text[start:cursor])
        bodies[match.group(1)] = body
    table = {}
    for version in FLARIAL_ORDER:
        body = bodies.get(version)
        if body is None:
            continue
        for op in re.finditer(r'(ADD_SIG|DEPRECATE_SIG)\(\s*"([^"]+)"\s*(?:,\s*"([^"]+)")?\s*\)', body):
            if op.group(1) == "DEPRECATE_SIG":
                table.pop(op.group(2), None)
            else:
                table[op.group(2)] = (op.group(3), version)
    return table


def main():
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("image", type=Path, nargs="?")
    source.add_argument("--pid", type=int)
    parser.add_argument("--flarial", type=Path)
    parser.add_argument("--pattern", action="append", default=[], help="NAME=HEX pattern")
    parser.add_argument("--only", help="only names containing this substring")
    args = parser.parse_args()

    code, text_rva = load_live_text(args.pid) if args.pid is not None else load_text(args.image)
    entries = {name: (sig, "oinkster") for name, sig in OINKSTER_PATTERNS.items()}
    if args.flarial:
        entries.update(flarial_table(args.flarial))
    for item in args.pattern:
        name, _, sig = item.partition("=")
        entries[name] = (sig, "cli")

    for name in sorted(entries):
        if args.only and args.only.lower() not in name.lower():
            continue
        sig, origin = entries[name]
        try:
            regex = compile_pattern(sig)
        except ValueError:
            print(f"invalid     {name}  [{origin}]")
            continue
        hits = []
        for match in regex.finditer(code):
            hits.append(match.start())
            if len(hits) > 3:
                break
        outcome = "missing" if not hits else "unique" if len(hits) == 1 else "ambiguous"
        where = " ".join(f"rva={text_rva + hit:#x}" for hit in hits[:3])
        print(f"{outcome:<10}  {name}  [{origin}]  {where}")
    sys.stdout.flush()


if __name__ == "__main__":
    main()
