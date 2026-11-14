#pragma once

#include "Oinkster/Events.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Camera keyframes for the replay editor: poses pinned to moments of the
// replay, and the smooth path through them that the viewer's camera follows
// while "Play camera path" is on. Pure logic; ReplayInjector drives the
// camera with it and saves it next to the replay.
namespace Oinkster::ReplayCameraPath {

// How the camera moves from a keyframe to the next one: along the curve at
// an even pace, slowing into and out of both ends, or not at all (it holds
// this shot, then cuts to the next).
enum class Ease : std::uint8_t { Smooth, EaseInOut, Cut };

struct Keyframe {
    std::uint64_t micros{};  // playback position, relative to the clip start
    Vec3 eye{};
    float yaw{};             // Bedrock convention, degrees
    float pitch{};
    Ease ease{Ease::Smooth};
};

struct Pose {
    Vec3 eye{};
    float yaw{};
    float pitch{};
};

// Keyframes closer together than this are one keyframe: adding at the same
// moment replaces it.
inline constexpr std::uint64_t SameMomentMicros = 100'000;

class Path {
public:
    // Adds a keyframe, or replaces one at (about) the same moment. Returns
    // its index in time order.
    std::size_t Add(const Keyframe& keyframe);
    void Remove(std::size_t index);
    // Moves a keyframe to another moment; returns its new index (nullopt if
    // another keyframe already sits there).
    std::optional<std::size_t> Retime(std::size_t index, std::uint64_t micros);
    void Clear() noexcept { keyframes_.clear(); }
    void SetEase(std::size_t index, Ease ease) noexcept {
        if (index < keyframes_.size()) keyframes_[index].ease = ease;
    }

    [[nodiscard]] const std::vector<Keyframe>& Keyframes() const noexcept { return keyframes_; }
    [[nodiscard]] bool Empty() const noexcept { return keyframes_.empty(); }
    [[nodiscard]] std::size_t Size() const noexcept { return keyframes_.size(); }

    // The camera at `micros`: a Catmull-Rom curve through the keyframes'
    // positions and angles (turning the short way round), held at the first
    // and last keyframe outside them. nullopt without keyframes.
    [[nodiscard]] std::optional<Pose> Evaluate(std::uint64_t micros) const;

    // {"version": 1, "keyframes": [{"t":..,"x":..,"y":..,"z":..,"yaw":..,"pitch":..,"ease":"smooth"}]}
    [[nodiscard]] std::string ToJson() const;
    [[nodiscard]] static std::optional<Path> FromJson(std::string_view text);

    friend bool operator==(const Path& left, const Path& right) noexcept;

private:
    std::vector<Keyframe> keyframes_;
};

}
