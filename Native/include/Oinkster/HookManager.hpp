#pragma once

namespace Oinkster::HookManager {

// Creates and enables a MinHook hook for `target`, writing the original
// function pointer into `*original`. On success the target is registered
// with this manager so a later DisableAll/RemoveAll tears every hook
// created through it down together, in the same order they were created -
// mirroring the centralized hook registry in Flarial dll-oss's
// src/Client/Hook/Manager.cpp.
[[nodiscard]] bool Create(void* target, void* detour, void** original) noexcept;

// Disables one registered hook without affecting unrelated subsystems. The
// target remains registered until Remove(target) so in-flight callbacks can
// drain before its trampoline is destroyed.
[[nodiscard]] bool Disable(void* target) noexcept;

// Removes one previously disabled hook and unregisters it.
[[nodiscard]] bool Remove(void* target) noexcept;

// Calls MH_DisableHook on every registered target, in creation order.
void DisableAll() noexcept;

// Calls MH_RemoveHook on every registered target, in creation order, then
// clears the registry. Callers that need to wait for in-flight hook
// callbacks to drain must do so between DisableAll() and RemoveAll().
void RemoveAll() noexcept;

}
