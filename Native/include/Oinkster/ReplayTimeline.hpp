#pragma once

#include "Oinkster/ReplayFile.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Turns a recorded .oinkreplay into something the Replays theater can play:
// camera samples, marks and the bounds of the travelled path. Pure logic; no
// rendering and no game access, so it is unit-tested in NativeTests.
namespace Oinkster::Replay {

struct CameraSample {
    std::uint64_t t{};
    double x{};
    double y{};
    double z{};
    std::optional<float> yaw;
    std::optional<float> pitch;
    std::optional<int> dimension;
    // The recording player's selected hotbar slot (0-8), when it was known.
    std::optional<int> hotbarSlot;
};

struct TimelineMark {
    std::uint64_t t{};
    std::string label;
};

struct PathPoint {
    float x{};
    float z{};
};

struct ReplayTimeline {
    std::uint64_t startedUnixMillis{};
    std::uint64_t durationMicros{};
    // Where the replay begins: a clip of a longer session starts here (its
    // "clip-start" marker); everything before it only rebuilds the world.
    std::uint64_t clipStartMicros{};
    bool cleanEnd{};
    std::vector<CameraSample> samples;
    std::vector<TimelineMark> marks;
    double minX{};
    double maxX{};
    double minZ{};
    double maxZ{};
    // Captured server packets: what in-world playback is built from.
    std::size_t packetCount{};
    std::uint64_t packetBytes{};
    bool hasStartGame{};
    // Ids of the first few packets, for checking a capture by eye.
    std::vector<std::uint32_t> firstPacketIds;
    std::string error;
};

struct PauseInterval {
    std::uint64_t startMicros{};
    std::uint64_t endMicros{};
};

// Maps capture timestamps onto playback time. Recording pauses are removed
// from the clock; packets captured while paused collapse onto the pause point
// so their final state is present when playback resumes.
struct PlaybackClock {
    std::uint64_t clipStartMicros{};
    std::uint64_t endMicros{};
    std::vector<PauseInterval> pauses;

    [[nodiscard]] std::uint64_t Map(std::uint64_t timestampMicros) const noexcept;
    [[nodiscard]] bool InsidePause(std::uint64_t timestampMicros) const noexcept;
};

[[nodiscard]] PlaybackClock BuildPlaybackClock(const ReadResult& replay);

// Malformed camera payloads (bad JSON, non-finite numbers) are skipped.
[[nodiscard]] ReplayTimeline BuildTimeline(const ReadResult& replay);

// Position at time t, linearly interpolated between samples and clamped to
// the first/last sample. Yaw follows the shortest arc; pitch and dimension
// take the nearer sample's value. nullopt when the timeline has no samples.
[[nodiscard]] std::optional<CameraSample> SampleAt(const ReplayTimeline& timeline, std::uint64_t t);

// Up to maxPoints evenly spaced path points from the clip start, for thumbnails.
[[nodiscard]] std::vector<PathPoint> DecimatePath(const ReplayTimeline& timeline, std::size_t maxPoints);

struct ReplaySummary {
    std::filesystem::path path;
    // Wall-clock time the clip starts, and its length.
    std::uint64_t startedUnixMillis{};
    std::uint64_t durationMicros{};
    std::size_t sampleCount{};
    std::size_t markCount{};
    bool cleanEnd{};
    std::uintmax_t bytes{};
    double minX{};
    double maxX{};
    double minZ{};
    double maxZ{};
    std::size_t packetCount{};
    std::uint64_t packetBytes{};
    bool hasStartGame{};
    std::vector<PathPoint> thumbnail;
    std::string error;
};

[[nodiscard]] ReplaySummary Summarize(const std::filesystem::path& path) noexcept;

}
