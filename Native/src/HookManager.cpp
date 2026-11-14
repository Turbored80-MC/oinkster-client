#include "Oinkster/HookManager.hpp"
#include <MinHook.h>
#include <algorithm>
#include <mutex>
#include <vector>

namespace Oinkster::HookManager {
namespace {
// Create() runs on the runtime thread during Attach, but the per-subsystem
// Disable()/Remove() teardowns are reachable from different threads (the
// render thread's eject path and the skin scan worker). The registry is a
// plain vector, so every access is serialized here.
std::mutex registryMutex;
std::vector<void*> targets;

[[nodiscard]] bool IsRegistered(void* target) noexcept {
    return std::ranges::find(targets, target) != targets.end();
}
}

bool Create(void* target, void* detour, void** original, std::string* error) noexcept {
    const auto fail = [error](const char* operation, MH_STATUS status) {
        if (!error) return;
        const char* detail = MH_StatusToString(status);
        *error = std::string(operation) + ": " + (detail ? detail : "unknown MinHook error");
    };
    if (!target || !detour || !original) {
        if (error) *error = "invalid hook target, detour, or original pointer";
        return false;
    }
    std::scoped_lock lock(registryMutex);
    const auto createStatus = MH_CreateHook(target, detour, original);
    if (createStatus != MH_OK) {
        fail("MH_CreateHook", createStatus);
        return false;
    }
    const auto enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK) {
        fail("MH_EnableHook", enableStatus);
        MH_RemoveHook(target);
        *original = nullptr;
        return false;
    }
    targets.push_back(target);
    return true;
}

bool Disable(void* target) noexcept {
    std::scoped_lock lock(registryMutex);
    if (!target || !IsRegistered(target)) return false;
    const auto result = MH_DisableHook(target);
    return result == MH_OK || result == MH_ERROR_DISABLED;
}

bool Remove(void* target) noexcept {
    std::scoped_lock lock(registryMutex);
    const auto found = std::ranges::find(targets, target);
    if (!target || found == targets.end()) return false;
    const auto result = MH_RemoveHook(target);
    if (result != MH_OK && result != MH_ERROR_NOT_CREATED) return false;
    targets.erase(found);
    return true;
}

void DisableAll() noexcept {
    std::scoped_lock lock(registryMutex);
    for (auto* target : targets) (void)MH_DisableHook(target);
}

void RemoveAll() noexcept {
    std::scoped_lock lock(registryMutex);
    for (auto* target : targets) (void)MH_RemoveHook(target);
    targets.clear();
}

}
