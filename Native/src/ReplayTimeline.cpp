#include "Oinkster/ReplayTimeline.hpp"

#include "Oinkster/Json.hpp"
#include "Oinkster/ReplayCapture.hpp"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace Oinkster::Replay {
namespace {

std::optional<double> FiniteNumber(const Json::Value& object, std::string_view key) {
    const auto* value = object.Find(key);
    if (!value) return std::nullopt;
    const auto* number = value->AsNumber();
    if (!number || !std::isfinite(*number)) return std::nullopt;
    return *number;
}

std::optional<CameraSample> ParseCamera(const Record& record) {
    const std::string_view text(reinterpret_cast<const char*>(record.payload.data()), record.payload.size());
    const auto parsed = Json::Parse(text);
    if (!parsed || !parsed->AsObject()) return std::nullopt;
    const auto x = FiniteNumber(*parsed, "x");
    const auto y = FiniteNumber(*parsed, "y");
    const auto z = FiniteNumber(*parsed, "z");
    if (!x || !y || !z) return std::nullopt;
    CameraSample sample{record.timestampMicros, *x, *y, *z, {}, {}, {}, {}};
    if (const auto yaw = FiniteNumber(*parsed, "yaw")) sample.yaw = static_cast<float>(*yaw);
    if (const auto pitch = FiniteNumber(*parsed, "pitch")) sample.pitch = static_cast<float>(*pitch);
    if (const auto dimension = FiniteNumber(*parsed, "dimension")) sample.dimension = static_cast<int>(*dimension);
    if (const auto slot = FiniteNumber(*parsed, "slot"); slot && *slot >= 0 && *slot <= 8) sample.hotbarSlot = static_cast<int>(*slot);
    return sample;
}

float LerpYaw(float from, float to, double amount) {
    float delta = std::fmod(to - from, 360.0F);
    if (delta > 180.0F) delta -= 360.0F;
    if (delta < -180.0F) delta += 360.0F;
    float result = from + delta * static_cast<float>(amount);
    while (result > 180.0F) result -= 360.0F;
    while (result <= -180.0F) result += 360.0F;
    return result;
}

}

std::uint64_t PlaybackClock::Map(std::uint64_t timestampMicros) const noexcept {
    std::uint64_t removed{};
    for (const auto& pause : pauses) {
        if (timestampMicros >= pause.endMicros) {
            removed += pause.endMicros - pause.startMicros;
            continue;
        }
        if (timestampMicros > pause.startMicros) removed += timestampMicros - pause.startMicros;
        break;
    }
    return timestampMicros >= removed ? timestampMicros - removed : 0;
}

bool PlaybackClock::InsidePause(std::uint64_t timestampMicros) const noexcept {
    return std::ranges::any_of(pauses, [timestampMicros](const PauseInterval& pause) {
        return timestampMicros > pause.startMicros && timestampMicros < pause.endMicros;
    });
}

PlaybackClock BuildPlaybackClock(const ReadResult& replay) {
    PlaybackClock clock;
    std::uint64_t rawEnd = replay.cleanEnd ? replay.endTimestampMicros : 0;
    std::vector<TimelineMark> controls;
    for (const auto& record : replay.records) {
        rawEnd = std::max(rawEnd, record.timestampMicros);
        if (record.kind != RecordKind::Marker) continue;
        std::string label(reinterpret_cast<const char*>(record.payload.data()), record.payload.size());
        if (label == "clip-start") clock.clipStartMicros = record.timestampMicros;
        else if (label == "pause" || label == "resume") controls.push_back({record.timestampMicros, std::move(label)});
    }
    std::ranges::stable_sort(controls, {}, &TimelineMark::t);
    std::optional<std::uint64_t> pausedAt;
    for (const auto& control : controls) {
        if (control.t < clock.clipStartMicros) continue;
        if (control.label == "pause" && !pausedAt) pausedAt = control.t;
        else if (control.label == "resume" && pausedAt) {
            clock.pauses.push_back({*pausedAt, std::max(*pausedAt, control.t)});
            pausedAt.reset();
        }
    }
    if (pausedAt) clock.pauses.push_back({*pausedAt, std::max(*pausedAt, rawEnd)});
    clock.endMicros = clock.Map(rawEnd);
    clock.clipStartMicros = clock.Map(clock.clipStartMicros);
    return clock;
}

ReplayTimeline BuildTimeline(const ReadResult& replay) {
    ReplayTimeline timeline;
    const auto clock = BuildPlaybackClock(replay);
    timeline.startedUnixMillis = replay.startedUnixMillis;
    timeline.cleanEnd = replay.cleanEnd;
    timeline.error = replay.error;
    timeline.clipStartMicros = clock.clipStartMicros;
    std::uint64_t lastTimestamp{};
    for (const auto& record : replay.records) {
        const auto playbackTimestamp = clock.Map(record.timestampMicros);
        lastTimestamp = std::max(lastTimestamp, playbackTimestamp);
        if (record.kind == RecordKind::Camera) {
            try {
                if (clock.InsidePause(record.timestampMicros)) continue;
                if (auto sample = ParseCamera(record)) {
                    sample->t = playbackTimestamp;
                    timeline.samples.push_back(*sample);
                }
            } catch (...) {
                // A payload the bounded reader cannot handle is just skipped.
            }
        } else if (record.kind == RecordKind::IncomingPacket) {
            ++timeline.packetCount;
            timeline.packetBytes += record.payload.size();
            if (const auto header = ReplayCapture::ParseHeader(record.payload)) {
                if (header->id == ReplayCapture::PacketId::StartGame) timeline.hasStartGame = true;
                if (timeline.firstPacketIds.size() < 12) timeline.firstPacketIds.push_back(header->id);
            }
        } else if (record.kind == RecordKind::Marker) {
            std::string label(reinterpret_cast<const char*>(record.payload.data()), record.payload.size());
            if (label != "clip-start" && label != "pause" && label != "resume") {
                timeline.marks.push_back({playbackTimestamp, std::move(label)});
            }
        }
    }
    std::ranges::stable_sort(timeline.samples, {}, &CameraSample::t);
    std::ranges::stable_sort(timeline.marks, {}, &TimelineMark::t);
    timeline.durationMicros = std::max(clock.endMicros, lastTimestamp);
    if (!timeline.samples.empty()) {
        timeline.minX = timeline.maxX = timeline.samples.front().x;
        timeline.minZ = timeline.maxZ = timeline.samples.front().z;
        for (const auto& sample : timeline.samples) {
            timeline.minX = std::min(timeline.minX, sample.x);
            timeline.maxX = std::max(timeline.maxX, sample.x);
            timeline.minZ = std::min(timeline.minZ, sample.z);
            timeline.maxZ = std::max(timeline.maxZ, sample.z);
        }
    }
    return timeline;
}

std::optional<CameraSample> SampleAt(const ReplayTimeline& timeline, std::uint64_t t) {
    const auto& samples = timeline.samples;
    if (samples.empty()) return std::nullopt;
    if (t <= samples.front().t) return samples.front();
    if (t >= samples.back().t) return samples.back();
    const auto after = std::ranges::upper_bound(samples, t, {}, &CameraSample::t);
    const auto& next = *after;
    const auto& previous = *(after - 1);
    const auto span = next.t - previous.t;
    const double amount = span == 0 ? 0.0 : static_cast<double>(t - previous.t) / static_cast<double>(span);
    // Never interpolate through a dimension change: the coordinates of two
    // dimensions are not on one line.
    if (previous.dimension != next.dimension) return amount < 0.5 ? previous : next;
    CameraSample result = amount < 0.5 ? previous : next;
    result.t = t;
    result.x = previous.x + (next.x - previous.x) * amount;
    result.y = previous.y + (next.y - previous.y) * amount;
    result.z = previous.z + (next.z - previous.z) * amount;
    if (previous.yaw && next.yaw) result.yaw = LerpYaw(*previous.yaw, *next.yaw, amount);
    if (previous.pitch && next.pitch) {
        result.pitch = *previous.pitch + (*next.pitch - *previous.pitch) * static_cast<float>(amount);
    }
    return result;
}

std::vector<PathPoint> DecimatePath(const ReplayTimeline& timeline, std::size_t maxPoints) {
    std::vector<PathPoint> points;
    const auto first = static_cast<std::size_t>(std::ranges::lower_bound(timeline.samples,
        timeline.clipStartMicros, {}, &CameraSample::t) - timeline.samples.begin());
    const auto count = timeline.samples.size() - first;
    if (count == 0 || maxPoints == 0) return points;
    const auto take = std::min(count, maxPoints);
    points.reserve(take);
    for (std::size_t index = 0; index < take; ++index) {
        const auto source = first + (take == 1 ? 0 : index * (count - 1) / (take - 1));
        const auto& sample = timeline.samples[source];
        points.push_back({static_cast<float>(sample.x), static_cast<float>(sample.z)});
    }
    return points;
}

ReplaySummary Summarize(const std::filesystem::path& path) noexcept {
    ReplaySummary summary;
    try {
        summary.path = path;
        std::error_code ignored;
        summary.bytes = std::filesystem::file_size(path, ignored);
        const auto timeline = BuildTimeline(Read(path));
        summary.startedUnixMillis = timeline.startedUnixMillis + timeline.clipStartMicros / 1000;
        summary.durationMicros = timeline.durationMicros > timeline.clipStartMicros
            ? timeline.durationMicros - timeline.clipStartMicros : 0;
        summary.sampleCount = timeline.samples.size();
        summary.cleanEnd = timeline.cleanEnd;
        summary.minX = timeline.minX;
        summary.maxX = timeline.maxX;
        summary.minZ = timeline.minZ;
        summary.maxZ = timeline.maxZ;
        summary.markCount = static_cast<std::size_t>(std::ranges::count_if(timeline.marks,
            [](const TimelineMark& mark) { return mark.label.starts_with("mark"); }));
        summary.thumbnail = DecimatePath(timeline, 64);
        summary.packetCount = timeline.packetCount;
        summary.packetBytes = timeline.packetBytes;
        summary.hasStartGame = timeline.hasStartGame;
        summary.error = timeline.error;
    } catch (const std::exception& exception) {
        summary.error = exception.what();
    } catch (...) {
        summary.error = "replay could not be summarized";
    }
    return summary;
}

}
