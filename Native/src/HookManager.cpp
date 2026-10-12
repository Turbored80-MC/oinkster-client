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

bool Create(void* target, void* detour, void** original) noexcept {
    if (!target || !detour || !original) return false;
    std::scoped_lock lock(registryMutex);
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) {
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
