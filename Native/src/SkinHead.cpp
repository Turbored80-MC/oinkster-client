#include "Oinkster/SkinHead.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace Oinkster {
namespace {

struct FaceOrigin { std::uint32_t x; std::uint32_t y; };

constexpr std::array<FaceOrigin, SkinHeadTexture::FaceCount> BaseOrigins{{
    {0, 8}, {8, 8}, {16, 8}, {24, 8}, {8, 0}, {16, 0}
}};
constexpr std::array<FaceOrigin, SkinHeadTexture::FaceCount> OverlayOrigins{{
    {32, 8}, {40, 8}, {48, 8}, {56, 8}, {40, 0}, {48, 0}
}};

std::uint32_t PackRgba(const std::uint8_t* pixel) noexcept {
    return static_cast<std::uint32_t>(pixel[0])
        | (static_cast<std::uint32_t>(pixel[1]) << 8U)
        | (static_cast<std::uint32_t>(pixel[2]) << 16U)
        | (static_cast<std::uint32_t>(pixel[3]) << 24U);
}

void ExtractFace(std::uint32_t width, std::uint32_t atlasScale, std::span<const std::uint8_t> pixels,
    FaceOrigin origin, SkinHeadTexture::Face& output) noexcept {
    const auto faceEdge = 8 * atlasScale;
    for (std::uint32_t y = 0; y < faceEdge; ++y) {
        for (std::uint32_t x = 0; x < faceEdge; ++x) {
            const auto sourceX = origin.x * atlasScale + x;
            const auto sourceY = origin.y * atlasScale + y;
            const auto offset = (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;
            output[static_cast<std::size_t>(y) * faceEdge + x] = PackRgba(pixels.data() + offset);
        }
    }
}

struct FloatColor { float red; float green; float blue; float alpha; };

FloatColor Premultiplied(std::uint32_t color) noexcept {
    const float alpha = static_cast<float>((color >> 24U) & 0xFFU) / 255.0F;
    return {static_cast<float>(color & 0xFFU) * alpha,
        static_cast<float>((color >> 8U) & 0xFFU) * alpha,
        static_cast<float>((color >> 16U) & 0xFFU) * alpha, alpha};
}

FloatColor Lerp(FloatColor a, FloatColor b, float amount) noexcept {
    return {a.red + (b.red - a.red) * amount, a.green + (b.green - a.green) * amount,
        a.blue + (b.blue - a.blue) * amount, a.alpha + (b.alpha - a.alpha) * amount};
}

}

bool ExtractSkinHead(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgbaPixels, SkinHeadTexture& result) noexcept {
    if (!((width == 64 && height == 64) || (width == 128 && height == 128))) return false;
    const auto expected = static_cast<std::size_t>(width) * height * 4;
    if (rgbaPixels.size() != expected) return false;

    SkinHeadTexture extracted{};
    const auto atlasScale = width / 64;
    extracted.faceEdge = 8 * atlasScale;
    for (std::size_t face = 0; face < SkinHeadTexture::FaceCount; ++face) {
        ExtractFace(width, atlasScale, rgbaPixels, BaseOrigins[face], extracted.base[face]);
        ExtractFace(width, atlasScale, rgbaPixels, OverlayOrigins[face], extracted.overlay[face]);
    }
    result = extracted;
    return true;
}

std::uint32_t SampleSkinFaceBilinear(const SkinHeadTexture::Face& face,
    std::uint32_t faceEdge, float u, float v) noexcept {
    if (faceEdge == 0 || faceEdge > SkinHeadTexture::MaxFaceEdge) return 0;
    const float sourceX = std::clamp(u, 0.0F, 1.0F) * static_cast<float>(faceEdge) - 0.5F;
    const float sourceY = std::clamp(v, 0.0F, 1.0F) * static_cast<float>(faceEdge) - 0.5F;
    const auto x0 = static_cast<std::uint32_t>(std::clamp(std::floor(sourceX), 0.0F,
        static_cast<float>(faceEdge - 1)));
    const auto y0 = static_cast<std::uint32_t>(std::clamp(std::floor(sourceY), 0.0F,
        static_cast<float>(faceEdge - 1)));
    const auto x1 = (std::min)(x0 + 1, faceEdge - 1);
    const auto y1 = (std::min)(y0 + 1, faceEdge - 1);
    const float tx = std::clamp(sourceX - std::floor(sourceX), 0.0F, 1.0F);
    const float ty = std::clamp(sourceY - std::floor(sourceY), 0.0F, 1.0F);
    const auto top = Lerp(Premultiplied(face[static_cast<std::size_t>(y0) * faceEdge + x0]),
        Premultiplied(face[static_cast<std::size_t>(y0) * faceEdge + x1]), tx);
    const auto bottom = Lerp(Premultiplied(face[static_cast<std::size_t>(y1) * faceEdge + x0]),
        Premultiplied(face[static_cast<std::size_t>(y1) * faceEdge + x1]), tx);
    const auto sampled = Lerp(top, bottom, ty);
    const auto alpha = static_cast<std::uint32_t>(std::clamp(sampled.alpha * 255.0F, 0.0F, 255.0F));
    if (alpha == 0 || sampled.alpha <= 0.0F) return 0;
    const auto red = static_cast<std::uint32_t>(std::clamp(sampled.red / sampled.alpha, 0.0F, 255.0F));
    const auto green = static_cast<std::uint32_t>(std::clamp(sampled.green / sampled.alpha, 0.0F, 255.0F));
    const auto blue = static_cast<std::uint32_t>(std::clamp(sampled.blue / sampled.alpha, 0.0F, 255.0F));
    return red | (green << 8U) | (blue << 16U) | (alpha << 24U);
}

float ParseGuiScale(std::string_view value) noexcept {
    float parsed = 0.65F;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !std::isfinite(parsed)) {
        return 0.65F;
    }
    return std::clamp(parsed, 0.50F, 1.00F);
}

float StepCriticallyDamped(float current, float& velocity, float target,
    float deltaSeconds, float smoothTime) noexcept {
    const float dt = std::clamp(deltaSeconds, 0.0F, 1.0F / 30.0F);
    const float duration = std::max(smoothTime, 0.001F);
    const float omega = 2.0F / duration;
    const float x = omega * dt;
    const float decay = 1.0F / (1.0F + x + 0.48F * x * x + 0.235F * x * x * x);
    const float displacement = current - target;
    const float temporary = (velocity + omega * displacement) * dt;
    velocity = (velocity - omega * temporary) * decay;
    return target + (displacement + temporary) * decay;
}

}
