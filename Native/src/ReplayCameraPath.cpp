#include "Oinkster/ReplayCameraPath.hpp"

#include "Oinkster/Json.hpp"
#include <algorithm>
#include <cmath>
#include <format>

namespace Oinkster::ReplayCameraPath {
namespace {

constexpr std::size_t MaxKeyframes = 4096;

double CatmullRom(double p0, double p1, double p2, double p3, double t) {
    const double t2 = t * t;
    const double t3 = t2 * t;
    return 0.5 * (2.0 * p1 + (p2 - p0) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 + (3.0 * p1 - p0 - 3.0 * p2 + p3) * t3);
}

// `to` moved by whole turns to within half a turn of `from`.
double Nearest(double from, double to) {
    double delta = std::fmod(to - from, 360.0);
    if (delta > 180.0) delta -= 360.0;
    if (delta < -180.0) delta += 360.0;
    return from + delta;
}

double WrapDegrees(double degrees) {
    degrees = std::fmod(degrees + 180.0, 360.0);
    if (degrees < 0.0) degrees += 360.0;
    return degrees - 180.0;
}

const char* EaseName(Ease ease) {
    switch (ease) {
    case Ease::Smooth: return "smooth";
    case Ease::EaseInOut: return "ease";
    case Ease::Cut: return "cut";
    }
    return "smooth";
}

Ease EaseFromName(std::string_view name) {
    if (name == "ease") return Ease::EaseInOut;
    if (name == "cut") return Ease::Cut;
    return Ease::Smooth;
}

bool Finite(const Keyframe& keyframe) {
    return std::isfinite(keyframe.eye.x) && std::isfinite(keyframe.eye.y) && std::isfinite(keyframe.eye.z)
        && std::isfinite(keyframe.yaw) && std::isfinite(keyframe.pitch);
}

}

std::size_t Path::Add(const Keyframe& keyframe) {
    const auto close = std::ranges::find_if(keyframes_, [&](const Keyframe& existing) {
        const auto gap = existing.micros > keyframe.micros ? existing.micros - keyframe.micros : keyframe.micros - existing.micros;
        return gap < SameMomentMicros;
    });
    if (close != keyframes_.end()) {
        const auto ease = close->ease;  // re-pinning a shot keeps how it moves on
        *close = keyframe;
        close->ease = ease;
        return static_cast<std::size_t>(close - keyframes_.begin());
    }
    if (keyframes_.size() >= MaxKeyframes) return keyframes_.size() - 1;
    const auto at = std::ranges::upper_bound(keyframes_, keyframe.micros, {}, &Keyframe::micros);
    return static_cast<std::size_t>(keyframes_.insert(at, keyframe) - keyframes_.begin());
}

void Path::Remove(std::size_t index) {
    if (index < keyframes_.size()) keyframes_.erase(keyframes_.begin() + static_cast<std::ptrdiff_t>(index));
}

std::optional<std::size_t> Path::Retime(std::size_t index, std::uint64_t micros) {
    if (index >= keyframes_.size()) return std::nullopt;
    for (std::size_t other = 0; other < keyframes_.size(); ++other) {
        if (other == index) continue;
        const auto t = keyframes_[other].micros;
        if ((t > micros ? t - micros : micros - t) < SameMomentMicros) return std::nullopt;
    }
    auto moved = keyframes_[index];
    moved.micros = micros;
    keyframes_.erase(keyframes_.begin() + static_cast<std::ptrdiff_t>(index));
    const auto at = std::ranges::upper_bound(keyframes_, micros, {}, &Keyframe::micros);
    return static_cast<std::size_t>(keyframes_.insert(at, moved) - keyframes_.begin());
}

std::optional<Pose> Path::Evaluate(std::uint64_t micros) const {
    if (keyframes_.empty()) return std::nullopt;
    const auto pose = [](const Keyframe& keyframe) { return Pose{keyframe.eye, keyframe.yaw, keyframe.pitch}; };
    if (micros <= keyframes_.front().micros) return pose(keyframes_.front());
    if (micros >= keyframes_.back().micros) return pose(keyframes_.back());
    const auto after = std::ranges::upper_bound(keyframes_, micros, {}, &Keyframe::micros);
    const auto i1 = static_cast<std::size_t>(after - keyframes_.begin()) - 1;
    const auto i2 = i1 + 1;
    const auto i0 = i1 == 0 ? i1 : i1 - 1;
    const auto i3 = std::min(i2 + 1, keyframes_.size() - 1);
    const auto& k0 = keyframes_[i0];
    const auto& k1 = keyframes_[i1];
    const auto& k2 = keyframes_[i2];
    const auto& k3 = keyframes_[i3];
    if (k1.ease == Ease::Cut) return pose(k1);
    const double span = static_cast<double>(k2.micros - k1.micros);
    double t = span <= 0.0 ? 0.0 : static_cast<double>(micros - k1.micros) / span;
    // Smoothstep: the curve is the same, the pace eases to a stop at both ends.
    if (k1.ease == Ease::EaseInOut) t = t * t * (3.0 - 2.0 * t);
    // Yaw unwrapped along the four so the curve turns the short way.
    const double y1 = k1.yaw;
    const double y0 = Nearest(y1, k0.yaw);
    const double y2 = Nearest(y1, k2.yaw);
    const double y3 = Nearest(y2, k3.yaw);
    Pose result;
    result.eye = {CatmullRom(k0.eye.x, k1.eye.x, k2.eye.x, k3.eye.x, t), CatmullRom(k0.eye.y, k1.eye.y, k2.eye.y, k3.eye.y, t),
        CatmullRom(k0.eye.z, k1.eye.z, k2.eye.z, k3.eye.z, t)};
    result.yaw = static_cast<float>(WrapDegrees(CatmullRom(y0, y1, y2, y3, t)));
    result.pitch = static_cast<float>(std::clamp(CatmullRom(k0.pitch, k1.pitch, k2.pitch, k3.pitch, t), -90.0, 90.0));
    return result;
}

std::string Path::ToJson() const {
    std::string json = "{\"version\": 1, \"keyframes\": [";
    for (std::size_t index = 0; index < keyframes_.size(); ++index) {
        const auto& keyframe = keyframes_[index];
        json += std::format("{}{{\"t\": {}, \"x\": {}, \"y\": {}, \"z\": {}, \"yaw\": {}, \"pitch\": {}, \"ease\": \"{}\"}}",
            index ? ", " : "", keyframe.micros, keyframe.eye.x, keyframe.eye.y, keyframe.eye.z, keyframe.yaw, keyframe.pitch,
            EaseName(keyframe.ease));
    }
    return json + "]}";
}

std::optional<Path> Path::FromJson(std::string_view text) {
    const auto parsed = Json::Parse(text);
    if (!parsed) return std::nullopt;
    const auto* list = parsed->Find("keyframes");
    if (!list || !list->AsArray()) return std::nullopt;
    Path path;
    for (const auto& item : *list->AsArray()) {
        const auto number = [&](std::string_view key) -> std::optional<double> {
            const auto* value = item.Find(key);
            const auto* number = value ? value->AsNumber() : nullptr;
            return number && std::isfinite(*number) ? std::optional<double>(*number) : std::nullopt;
        };
        const auto t = number("t"), x = number("x"), y = number("y"), z = number("z"), yaw = number("yaw"), pitch = number("pitch");
        if (!t || !x || !y || !z || !yaw || !pitch || *t < 0.0) continue;
        Keyframe keyframe{static_cast<std::uint64_t>(*t), {*x, *y, *z}, static_cast<float>(*yaw), static_cast<float>(*pitch)};
        if (const auto* ease = item.Find("ease"); ease && ease->AsString()) keyframe.ease = EaseFromName(*ease->AsString());
        if (Finite(keyframe)) path.Add(keyframe);
    }
    return path;
}

bool operator==(const Path& left, const Path& right) noexcept {
    return std::ranges::equal(left.keyframes_, right.keyframes_, [](const Keyframe& a, const Keyframe& b) {
        return a.micros == b.micros && a.eye.x == b.eye.x && a.eye.y == b.eye.y && a.eye.z == b.eye.z && a.yaw == b.yaw
            && a.pitch == b.pitch && a.ease == b.ease;
    });
}

}
