#pragma once

#include <algorithm>
#include <cmath>

// Pure camera math for the presentation adapter. The game's CameraComponent
// keeps a camera-to-world rotation quaternion (x, y, z, w): the camera update
// builds glm::mat4_cast(q) and inverts it into the view matrix, and the
// projection is right-handed, so the camera looks down -Z in its own space.
namespace Oinkster::Modules::CameraMath {

struct Float3 {
    float x{};
    float y{};
    float z{};
};

struct Quat {
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

inline constexpr float Pi = 3.14159265358979323846F;
// Freelook never takes the view closer than this to straight up or down,
// where yaw would be undefined.
inline constexpr float MaxPitchRadians = 89.0F * Pi / 180.0F;

[[nodiscard]] inline Float3 operator+(Float3 a, Float3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] inline Float3 operator-(Float3 a, Float3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] inline Float3 operator*(Float3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] inline float Dot(Float3 a, Float3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] inline Float3 Cross(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline float Length(Float3 a) noexcept { return std::sqrt(Dot(a, a)); }
[[nodiscard]] inline Float3 Normalize(Float3 a) noexcept {
    const float length = Length(a);
    return length > 1e-6F ? a * (1.0F / length) : Float3{};
}

[[nodiscard]] inline bool IsFinite(const Quat& q) noexcept {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}
[[nodiscard]] inline float Dot(const Quat& a, const Quat& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
[[nodiscard]] inline Quat Normalize(const Quat& q) noexcept {
    const float length = std::sqrt(Dot(q, q));
    return length > 1e-6F ? Quat{q.x / length, q.y / length, q.z / length, q.w / length} : Quat{};
}
[[nodiscard]] inline Quat Multiply(const Quat& a, const Quat& b) noexcept {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
[[nodiscard]] inline Quat AxisAngle(Float3 axis, float radians) noexcept {
    const Float3 unit = Normalize(axis);
    const float s = std::sin(radians * 0.5F);
    return {unit.x * s, unit.y * s, unit.z * s, std::cos(radians * 0.5F)};
}
[[nodiscard]] inline Float3 Rotate(const Quat& q, Float3 v) noexcept {
    const Float3 u{q.x, q.y, q.z};
    const Float3 t = Cross(u, v) * 2.0F;
    return v + t * q.w + Cross(u, t);
}

[[nodiscard]] inline Float3 Forward(const Quat& q) noexcept { return Rotate(q, {0.0F, 0.0F, -1.0F}); }
[[nodiscard]] inline Float3 Right(const Quat& q) noexcept { return Rotate(q, {1.0F, 0.0F, 0.0F}); }
[[nodiscard]] inline Float3 Up(const Quat& q) noexcept { return Rotate(q, {0.0F, 1.0F, 0.0F}); }

// Shortest-path spherical interpolation, t in [0, 1].
[[nodiscard]] inline Quat Slerp(const Quat& from, const Quat& to, float t) noexcept {
    t = std::clamp(t, 0.0F, 1.0F);
    Quat end = to;
    float cosine = Dot(from, to);
    if (cosine < 0.0F) {
        end = {-to.x, -to.y, -to.z, -to.w};
        cosine = -cosine;
    }
    if (cosine > 0.9995F) {
        return Normalize(Quat{from.x + (end.x - from.x) * t, from.y + (end.y - from.y) * t,
            from.z + (end.z - from.z) * t, from.w + (end.w - from.w) * t});
    }
    const float angle = std::acos(std::clamp(cosine, -1.0F, 1.0F));
    const float sine = std::sin(angle);
    const float a = std::sin((1.0F - t) * angle) / sine;
    const float b = std::sin(t * angle) / sine;
    return {from.x * a + end.x * b, from.y * a + end.y * b, from.z * a + end.z * b, from.w * a + end.w * b};
}

// Rotation whose columns are the given orthonormal camera axes.
[[nodiscard]] inline Quat FromAxes(Float3 right, Float3 up, Float3 back) noexcept {
    const float trace = right.x + up.y + back.z;
    Quat q;
    if (trace > 0.0F) {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        q = {(up.z - back.y) / s, (back.x - right.z) / s, (right.y - up.x) / s, 0.25F * s};
    } else if (right.x > up.y && right.x > back.z) {
        const float s = std::sqrt(1.0F + right.x - up.y - back.z) * 2.0F;
        q = {0.25F * s, (up.x + right.y) / s, (back.x + right.z) / s, (up.z - back.y) / s};
    } else if (up.y > back.z) {
        const float s = std::sqrt(1.0F + up.y - right.x - back.z) * 2.0F;
        q = {(up.x + right.y) / s, 0.25F * s, (back.y + up.z) / s, (back.x - right.z) / s};
    } else {
        const float s = std::sqrt(1.0F + back.z - right.x - up.y) * 2.0F;
        q = {(back.x + right.z) / s, (back.y + up.z) / s, 0.25F * s, (right.y - up.x) / s};
    }
    return Normalize(q);
}

// The same view direction with the camera's right axis level (no roll).
// Looking straight up or down there is no level right axis; q is returned.
[[nodiscard]] inline Quat RollFree(const Quat& q) noexcept {
    const Float3 forward = Normalize(Forward(q));
    Float3 right = Cross(forward, {0.0F, 1.0F, 0.0F});
    if (Length(right) < 1e-3F) return q;
    right = Normalize(right);
    const Float3 up = Cross(right, forward);
    const Quat level = FromAxes(right, up, forward * -1.0F);
    // Keep the representation on q's side so later slerps take the short way.
    return Dot(level, q) < 0.0F ? Quat{-level.x, -level.y, -level.z, -level.w} : level;
}

// Roll scaled by `strength`: 0 removes it (hurt-camera tilt), 1 keeps it.
[[nodiscard]] inline Quat ScaleRoll(const Quat& q, float strength) noexcept {
    return Slerp(RollFree(q), q, strength);
}

// The current view pitch in radians (positive looking up).
[[nodiscard]] inline float Pitch(const Quat& q) noexcept {
    return std::asin(std::clamp(Normalize(Forward(q)).y, -1.0F, 1.0F));
}

// Turns the view by `yaw` around world up and `pitch` around the camera's
// right axis (positive looks left / up), keeping the result inside
// +-MaxPitchRadians.
[[nodiscard]] inline Quat OffsetYawPitch(const Quat& q, float yaw, float pitch) noexcept {
    const float current = Pitch(q);
    const float target = std::clamp(current + pitch, -MaxPitchRadians, MaxPitchRadians);
    const Quat pitched = Multiply(q, AxisAngle({1.0F, 0.0F, 0.0F}, target - current));
    return Normalize(Multiply(AxisAngle({0.0F, 1.0F, 0.0F}, yaw), pitched));
}

// Distance from `camera` to the point on its view ray nearest `pivot`.
[[nodiscard]] inline float DistanceAlongView(Float3 camera, Float3 forward, Float3 pivot) noexcept {
    return Dot(pivot - camera, Normalize(forward));
}

}
