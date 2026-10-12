#pragma once

#include "Oinkster/Events.hpp"
#include "Oinkster/Modules/RollingWindow.hpp"
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

// Shared render-performance metrics.
//
// Definitions (also in Native/README.md):
//   * Frame time   - wall time between consecutive presented frames, as
//                    measured on the Present hook.
//   * FPS          - frames in the smoothing window / their summed frame time
//                    (a time-weighted rate, not a mean of per-frame FPS).
//   * Average FPS  - same formula over the statistics window.
//   * Minimum FPS  - 1 / the longest single frame time in the window.
//   * 1% low FPS   - 1 / the mean of the slowest 1% of frame times in the
//                    window (at least one frame), reported only once the
//                    window holds >= 100 frames. This is NOT the lowest
//                    single sample.
namespace Oinkster::Modules {

struct FrameStatistics {
    std::size_t frames{};
    double averageFps{};
    double minimumFps{};
    std::optional<double> onePercentLowFps;
    double averageFrameMs{};
    double maximumFrameMs{};
};

inline constexpr std::size_t MinimumFramesForOnePercentLow = 100;
inline constexpr std::size_t FrameHistoryCapacity = 8192;

// Pure: statistics over frame times in seconds.
[[nodiscard]] FrameStatistics ComputeFrameStatistics(std::span<const double> frameSeconds);

// Thread-safe, bounded history of presented frames. Several performance
// modules share one instance; Record deduplicates on RenderEvent::frameIndex
// so each frame is counted exactly once however many modules forward it.
class FrameHistory {
public:
    explicit FrameHistory(std::size_t capacity = FrameHistoryCapacity) : samples_(capacity) {}

    // Returns false for a duplicate or out-of-order frame index.
    bool Record(const RenderEvent& event);
    void Clear();

    // Smoothed FPS / frame time over the trailing `window` ending at the
    // newest frame. nullopt when no frame has been recorded.
    [[nodiscard]] std::optional<double> Fps(SteadyClock::duration window) const;
    [[nodiscard]] std::optional<double> FrameMilliseconds(SteadyClock::duration window) const;
    [[nodiscard]] FrameStatistics Statistics(SteadyClock::duration window) const;
    // Newest `count` frame times in milliseconds, oldest first.
    [[nodiscard]] std::vector<float> RecentFrameMilliseconds(std::size_t count) const;
    [[nodiscard]] std::size_t Size() const;

private:
    struct Sample { SteadyTime at; double seconds; };
    [[nodiscard]] std::vector<double> WindowLocked(SteadyClock::duration window) const;

    mutable std::mutex mutex_;
    RingBuffer<Sample> samples_;
    std::optional<std::uint64_t> lastFrameIndex_;
};

// Working-set sampler shared by Process memory and Performance summary.
// Queries the OS at most once per refresh interval, never on every frame.
class ProcessMemorySampler {
public:
    using Query = std::function<std::optional<std::uint64_t>()>;
    explicit ProcessMemorySampler(Query query = {}, SteadyClock::duration refresh = std::chrono::milliseconds(500));
    [[nodiscard]] std::optional<std::uint64_t> WorkingSetBytes(SteadyTime now);

private:
    std::mutex mutex_;
    Query query_;
    SteadyClock::duration refresh_;
    std::optional<SteadyTime> lastQuery_;
    std::optional<std::uint64_t> cached_;
};

[[nodiscard]] std::optional<std::uint64_t> QueryCurrentProcessWorkingSet() noexcept;

}
