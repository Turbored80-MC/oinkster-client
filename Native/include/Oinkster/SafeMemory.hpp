#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Guarded reads of the game's address space.
//
// Nothing here ever writes to the game, and every read is both range-checked
// (VirtualQuery) and SEH-guarded: another thread can free a region between the
// check and the read, and a torn read must not take Minecraft down with us.
//
// This is the discipline PlayerSkinTelemetry established for its reverse-
// engineered offset chains; it lives here so every subsystem that walks game
// memory (PlayerSkinTelemetry, WorldTelemetry) shares one implementation
// rather than each re-deriving it - or, as WorldTelemetry previously did,
// skipping it entirely and memcpy'ing at a hard-coded offset.
// ---------------------------------------------------------------------------
namespace Oinkster::SafeMemory {

[[nodiscard]] inline bool IsReadable(const void* address, std::size_t bytes) noexcept {
    if (!address || bytes == 0) return false;
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(address, &information, sizeof(information)) != sizeof(information)) return false;
    if (information.State != MEM_COMMIT || (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(information.BaseAddress) + information.RegionSize;
    return start <= regionEnd && bytes <= regionEnd - start;
}

// POD-only so the function needs no unwinding (a requirement for __try).
[[nodiscard]] inline bool CopyGuarded(void* destination, const void* source, std::size_t bytes) noexcept {
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
[[nodiscard]] bool ReadField(const void* base, std::size_t offset, T& value) noexcept {
    if (!base) return false;
    const auto* address = static_cast<const std::uint8_t*>(base) + offset;
    if (!IsReadable(address, sizeof(T))) return false;
    return CopyGuarded(&value, address, sizeof(T));
}

// Reads slot `index` out of the vtable `instance` points at, validating both
// the vtable pointer and the slot itself before handing back a callable
// address. Returns nullptr rather than faulting if either read is bad - which
// is what makes a stale offset on a future Bedrock patch a missing feature
// instead of an access violation inside the game's render thread.
[[nodiscard]] inline void* ReadVirtualSlot(const void* instance, std::size_t index) noexcept {
    void* vtable{};
    if (!ReadField(instance, 0, vtable) || !vtable) return nullptr;
    void* slot{};
    if (!ReadField(vtable, index * sizeof(void*), slot)) return nullptr;
    // A virtual function must live in committed, executable memory; a garbage
    // pointer that happens to be readable would otherwise be called outright.
    if (!slot) return nullptr;
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(slot, &information, sizeof(information)) != sizeof(information)) return nullptr;
    if (information.State != MEM_COMMIT) return nullptr;
    constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((information.Protect & executable) == 0) return nullptr;
    return slot;
}

}
