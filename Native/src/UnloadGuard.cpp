#include "Oinkster/UnloadGuard.hpp"
#include <atomic>
#include <mutex>

namespace Oinkster::UnloadGuard {
namespace {
std::atomic_bool blocked{};
std::mutex reasonMutex;
std::string reason;
}

void Block(std::string value) noexcept {
    try {
        std::scoped_lock lock(reasonMutex);
        if (!blocked.exchange(true, std::memory_order_acq_rel)) reason = std::move(value);
    } catch (...) {
        // Even if recording the reason fails, the block itself must stick.
        blocked.store(true, std::memory_order_release);
    }
}

bool Blocked() noexcept { return blocked.load(std::memory_order_acquire); }

std::string Reason() {
    std::scoped_lock lock(reasonMutex);
    return reason;
}

void ResetForTesting() noexcept {
    try {
        std::scoped_lock lock(reasonMutex);
        reason.clear();
    } catch (...) {}
    blocked.store(false, std::memory_order_release);
}

}
