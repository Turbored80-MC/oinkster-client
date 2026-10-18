#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace Oinkster {

enum class SkinFace : std::size_t { Right, Front, Left, Back, Top, Bottom, Count };

struct SkinHeadTexture {
    static constexpr std::size_t MaxFaceEdge = 16;
    static constexpr std::size_t FacePixels = MaxFaceEdge * MaxFaceEdge;
    static constexpr std::size_t FaceCount = static_cast<std::size_t>(SkinFace::Count);
    using Face = std::array<std::uint32_t, FacePixels>;

    std::uint32_t faceEdge{8};
    std::array<Face, FaceCount> base{};
    std::array<Face, FaceCount> overlay{};
};

[[nodiscard]] bool ExtractSkinHead(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgbaPixels, SkinHeadTexture& result) noexcept;
[[nodiscard]] std::uint32_t SampleSkinFaceBilinear(const SkinHeadTexture::Face& face,
    std::uint32_t faceEdge, float u, float v) noexcept;
[[nodiscard]] float ParseGuiScale(std::string_view value) noexcept;
[[nodiscard]] float StepCriticallyDamped(float current, float& velocity, float target,
    float deltaSeconds, float smoothTime = 0.12F) noexcept;

}
