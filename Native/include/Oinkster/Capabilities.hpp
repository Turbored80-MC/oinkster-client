#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Oinkster {

// Every source of data or behavior a module can depend on. A module is
// available only while every capability it requires is Available; nothing
// is inferred from a module's category any more.
//
// Ids are stable: they appear in Assets/module-catalog.json ("capabilities"),
// in the status pipe, and in diagnostics.
enum class Capability : std::uint8_t {
    RenderMetrics,        // Present-hook frame timing (Oinkster's own DX12 hook)
    FrameComposition,     // Drawing into / copying the presented backbuffer (own DX12 hook)
    LocalInput,           // Keyboard/mouse messages seen by the subclassed Minecraft window
    InputSynthesis,       // Posting ordinary key presses back to that same window
    InputFiltering,       // Suppressing reviewed physical mouse events without synthesizing replacements
    DcPreventPolicy,      // Current session policy permits DC Prevent
    CpsCapPolicy,         // Current session policy permits CPS Cap
    LocalPlayerPosition,  // World telemetry: local camera position
    LocalPlayerRotation,  // World telemetry: local yaw/pitch
    Inventory,
    Equipment,
    Effects,
    NetworkMetrics,
    ServerState,
    ResourcePacks,
    CombatEvents,
    SelectedBlockRender,
    DamageRender,
    SoundEvents,
    ChatEvents,
    CameraPresentation,
    CameraRotation,
    CameraMotion,
    ThirdPersonPresentation,
    LightingPresentation,
    TimePresentation,
    WeatherPresentation,
    ScreenEffectsPresentation,
    HandPresentation,
    PacketCapture,        // Decoded incoming game packets (NetworkConnection::receivePacket)
    PerspectivePresentation,
    PacketSend,
    Count
};

inline constexpr std::size_t CapabilityCount = static_cast<std::size_t>(Capability::Count);

enum class CapabilityState : std::uint8_t {
    // Can still change once the adapter attaches (e.g. a signature scan has
    // not run yet). Treated exactly like Unavailable for module availability.
    Pending,
    Available,
    Unavailable
};

struct CapabilityStatus {
    CapabilityState state{CapabilityState::Unavailable};
    std::wstring reason;
    [[nodiscard]] bool IsAvailable() const noexcept { return state == CapabilityState::Available; }
};

[[nodiscard]] std::string_view CapabilityId(Capability capability) noexcept;
[[nodiscard]] std::wstring_view CapabilityName(Capability capability) noexcept;
[[nodiscard]] std::optional<Capability> ParseCapabilityId(std::string_view id) noexcept;
[[nodiscard]] std::string_view CapabilityStateId(CapabilityState state) noexcept;

// The status every capability starts with before any adapter reports in.
// Capabilities provided by Oinkster's own DX12/window hooks start Available
// (the runtime refuses to run at all if those hooks fail to install). Every
// game-memory-backed capability starts Pending or Unavailable with a reason
// naming exactly what is missing.
[[nodiscard]] CapabilityStatus DefaultCapabilityStatus(Capability capability);

// Whether the shipped client contains a reviewed implementation for this
// capability on SupportedBuild. This is what Assets/module-catalog.json's
// static "available" flag reflects. Runtime activation still requires the
// adapter to validate its signatures and report Available.
[[nodiscard]] bool IsCapabilitySupported(Capability capability) noexcept;

// Kept as a source-compatible alias for older call sites. New code should use
// IsCapabilitySupported so launcher support is not confused with live state.
[[nodiscard]] inline bool IsStaticallyAvailable(Capability capability) noexcept {
    return IsCapabilitySupported(capability);
}

}
