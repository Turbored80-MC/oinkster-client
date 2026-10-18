#pragma once

#include "Oinkster/Events.hpp"
#include "Oinkster/Modules/RollingWindow.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Oinkster::Modules {

// Yaw normalized to [0, 360).
[[nodiscard]] float NormalizeYaw360(float yaw) noexcept;
// Yaw normalized to (-180, 180] - the range Bedrock reports.
[[nodiscard]] float NormalizeYaw180(float yaw) noexcept;
// Bedrock convention: 0 = South, 90 = West, 180 = North, 270 = East.
[[nodiscard]] std::string_view CompassDirection(float yaw, bool intercardinal) noexcept;
// Dominant horizontal axis the player faces: "+Z", "-X", ...
[[nodiscard]] std::string_view FacingAxis(float yaw) noexcept;

// A world position is plausible when every component is finite, |x|,|z| are
// within Bedrock's 30,000,000-block world border, and y is within a generous
// band around the build limits. Anything else is a misread, never data.
[[nodiscard]] bool IsPlausibleWorldPosition(const Vec3& position) noexcept;

// Pitch must be finite and within [-90, 90]; yaw finite and within one turn
// either way. Anything else means the rotation offset was misread.
[[nodiscard]] bool IsPlausibleRotation(float pitchDegrees, float yawDegrees) noexcept;

// Bedrock's standing eye height. World telemetry reads the first-person
// render camera, which sits this far above the player's feet.
inline constexpr double PlayerEyeHeight = 1.62;
[[nodiscard]] Vec3 FeetFromEye(const Vec3& eye) noexcept;

// One coordinate axis as text. Block mode floors to the containing block,
// matching Minecraft's own "Show Coordinates" readout (0.5 -> 0, -2.98 -> -3).
[[nodiscard]] std::string FormatCoordinate(double value, bool block, int precision);

// Movement speed from timestamped positions.
//
// Speed is path length / elapsed time over a trailing window. The estimator
// resets (and reports why) instead of reporting a spike when:
//   * the implied speed of one step exceeds maxPlausibleSpeed (teleport,
//     respawn, /tp, chunk-load correction),
//   * the gap between samples exceeds maxGap (paused, loading, alt-tab),
//   * the dimension changes, or the position is implausible.
class SpeedEstimator {
public:
    struct Config {
        SteadyClock::duration window{std::chrono::milliseconds(500)};
        SteadyClock::duration maxGap{std::chrono::seconds(1)};
        double maxPlausibleSpeed{100.0};  // blocks per second
    };
    enum class Result { Accepted, Started, ResetTeleport, ResetGap, ResetDimension, Rejected };

    SpeedEstimator() : SpeedEstimator(Config{}) {}
    explicit SpeedEstimator(Config config) : config_(config), samples_(256) {}

    Result Add(const PlayerTransform& transform);
    void Clear() noexcept { samples_.Clear(); dimension_.reset(); }
    // Blocks per second; nullopt until two samples span a measurable interval.
    [[nodiscard]] std::optional<double> Speed(bool horizontalOnly) const;

private:
    struct Sample { SteadyTime at; Vec3 position; };
    Config config_;
    RingBuffer<Sample> samples_;
    std::optional<int> dimension_;
};

// RakNet keeps the last few round-trip times per remote system; this value
// marks a slot that has not been filled yet.
inline constexpr std::uint16_t RakNetEmptyPing = 0xFFFF;
// Mean of the filled slots (what RakPeer::GetAveragePing reports), or nullopt
// when none is filled.
[[nodiscard]] std::optional<std::uint32_t> AveragePing(std::span<const std::uint16_t> samples) noexcept;

// Session kind from the connected remote's address: loopback is the local
// world, private/link-local ranges are LAN, anything else is a server. The
// address itself never leaves the adapter.
[[nodiscard]] SessionKind ClassifyRemoteIpv4(const std::array<std::uint8_t, 4>& address) noexcept;
[[nodiscard]] SessionKind ClassifyRemoteIpv6(const std::array<std::uint8_t, 16>& address) noexcept;

// A world name as the HUD shows it: UTF-8 decoded, formatting codes removed,
// whitespace trimmed, at most MaxWorldNameLength characters.
inline constexpr std::size_t MaxWorldNameLength = 64;
[[nodiscard]] std::wstring CleanWorldName(std::string_view utf8);

// Latest validated ping; stale samples read as unavailable.
class PingTracker {
public:
    void Update(const NetworkMetrics& metrics) noexcept;
    [[nodiscard]] std::optional<std::uint32_t> Current(SteadyTime now, SteadyClock::duration staleAfter) const noexcept;
    void Clear() noexcept { latest_.reset(); }

private:
    std::optional<NetworkMetrics> latest_;
};

}
