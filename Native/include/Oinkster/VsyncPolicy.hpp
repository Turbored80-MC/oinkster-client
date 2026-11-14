#pragma once

#include <cstdint>

namespace Oinkster {

enum class VsyncState : std::uint8_t {
    Enabled,
    Disabled,
    RestartRequired,
    Unsupported
};

struct VsyncPresentInput {
    bool disableRequested{};
    bool systemAllowsTearing{};
    bool chainAllowsTearing{};
    bool exclusiveFullscreen{};
    bool testPresent{};
    std::uint32_t interval{};
    std::uint32_t flags{};
};

struct VsyncPresentDecision {
    VsyncState state{VsyncState::Enabled};
    std::uint32_t interval{};
    std::uint32_t flags{};
};

// DXGI_PRESENT_ALLOW_TEARING is intentionally supplied by the caller. This
// keeps the policy independently testable without bringing DXGI into tests.
[[nodiscard]] constexpr VsyncPresentDecision DecideVsyncPresent(
    const VsyncPresentInput input, const std::uint32_t allowTearingFlag) noexcept {
    VsyncPresentDecision result{VsyncState::Enabled, input.interval, input.flags};
    if (!input.disableRequested) return result;
    if (!input.systemAllowsTearing) {
        result.state = VsyncState::Unsupported;
        return result;
    }
    if (!input.chainAllowsTearing) {
        result.state = VsyncState::RestartRequired;
        return result;
    }
    result.state = VsyncState::Disabled;
    if (!input.testPresent) {
        result.interval = 0;
        if (!input.exclusiveFullscreen) result.flags |= allowTearingFlag;
    }
    return result;
}

}
