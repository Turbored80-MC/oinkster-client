#include "Oinkster/Modules/FrameStatistics.hpp"
#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace Oinkster::Modules {

FrameStatistics ComputeFrameStatistics(std::span<const double> frameSeconds) {
    FrameStatistics result{};
    result.frames = frameSeconds.size();
    if (frameSeconds.empty()) return result;
    const double total = std::accumulate(frameSeconds.begin(), frameSeconds.end(), 0.0);
    const double longest = *std::ranges::max_element(frameSeconds);
    result.averageFps = total > 0.0 ? static_cast<double>(frameSeconds.size()) / total : 0.0;
    result.minimumFps = longest > 0.0 ? 1.0 / longest : 0.0;
    result.averageFrameMs = total * 1000.0 / static_cast<double>(frameSeconds.size());
    result.maximumFrameMs = longest * 1000.0;
    if (frameSeconds.size() >= MinimumFramesForOnePercentLow) {
        std::vector<double> sorted(frameSeconds.begin(), frameSeconds.end());
        const std::size_t slowest = std::max<std::size_t>(1, (sorted.size() + 99) / 100);
        std::ranges::nth_element(sorted, sorted.begin() + static_cast<std::ptrdiff_t>(slowest - 1), std::greater<>{});
        const double slowSum = std::accumulate(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(slowest), 0.0);
        const double slowMean = slowSum / static_cast<double>(slowest);
        if (slowMean > 0.0) result.onePercentLowFps = 1.0 / slowMean;
    }
    return result;
}

bool FrameHistory::Record(const RenderEvent& event) {
    std::scoped_lock lock(mutex_);
    if (lastFrameIndex_ && event.frameIndex <= *lastFrameIndex_) return false;
    lastFrameIndex_ = event.frameIndex;
    if (!std::isfinite(event.deltaSeconds) || event.deltaSeconds <= 0.0F) return false;
    // A multi-second stall (alt-tab, breakpoint, loading screen) is real but
    // would poison every average for the whole window; clamp it to 1 s.
    samples_.Push({event.presentedAt, std::min(static_cast<double>(event.deltaSeconds), 1.0)});
    return true;
}

void FrameHistory::Clear() {
    std::scoped_lock lock(mutex_);
    samples_.Clear();
    lastFrameIndex_.reset();
}

std::vector<double> FrameHistory::WindowLocked(SteadyClock::duration window) const {
    std::vector<double> result;
    if (samples_.Empty()) return result;
    const auto newest = samples_.Back().at;
    for (std::size_t index = samples_.Size(); index-- > 0;) {
        const auto& sample = samples_.At(index);
        if (newest - sample.at >= window && !result.empty()) break;
        result.push_back(sample.seconds);
    }
    std::ranges::reverse(result);
    return result;
}

std::optional<double> FrameHistory::Fps(SteadyClock::duration window) const {
    std::scoped_lock lock(mutex_);
    const auto frames = WindowLocked(window);
    if (frames.empty()) return std::nullopt;
    const double total = std::accumulate(frames.begin(), frames.end(), 0.0);
    return total > 0.0 ? std::optional(static_cast<double>(frames.size()) / total) : std::nullopt;
}

std::optional<double> FrameHistory::FrameMilliseconds(SteadyClock::duration window) const {
    std::scoped_lock lock(mutex_);
    const auto frames = WindowLocked(window);
    if (frames.empty()) return std::nullopt;
    return std::accumulate(frames.begin(), frames.end(), 0.0) * 1000.0 / static_cast<double>(frames.size());
}

FrameStatistics FrameHistory::Statistics(SteadyClock::duration window) const {
    std::scoped_lock lock(mutex_);
    const auto frames = WindowLocked(window);
    return ComputeFrameStatistics(frames);
}

std::vector<float> FrameHistory::RecentFrameMilliseconds(std::size_t count) const {
    std::scoped_lock lock(mutex_);
    const std::size_t taken = std::min(count, samples_.Size());
    std::vector<float> result;
    result.reserve(taken);
    for (std::size_t index = samples_.Size() - taken; index < samples_.Size(); ++index) {
        result.push_back(static_cast<float>(samples_.At(index).seconds * 1000.0));
    }
    return result;
}

std::size_t FrameHistory::Size() const {
    std::scoped_lock lock(mutex_);
    return samples_.Size();
}

ProcessMemorySampler::ProcessMemorySampler(Query query, SteadyClock::duration refresh)
    : query_(query ? std::move(query) : Query(&QueryCurrentProcessWorkingSet)), refresh_(refresh) {}

std::optional<std::uint64_t> ProcessMemorySampler::WorkingSetBytes(SteadyTime now) {
    std::scoped_lock lock(mutex_);
    if (!lastQuery_ || now - *lastQuery_ >= refresh_ || now < *lastQuery_) {
        lastQuery_ = now;
        try { cached_ = query_(); } catch (...) { cached_.reset(); }
    }
    return cached_;
}

std::optional<std::uint64_t> QueryCurrentProcessWorkingSet() noexcept {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return std::nullopt;
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
}

}
