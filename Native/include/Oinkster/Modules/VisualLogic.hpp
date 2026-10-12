#pragma once

#include "Oinkster/Module.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace Oinkster { class ModuleManager; }

namespace Oinkster::Modules {

struct CrosshairStyle {
    float length{6.0F};      // arm length in px
    float gap{3.0F};         // distance from center to each arm
    float thickness{2.0F};
    std::uint32_t color{0xFFFFFFFFU};
    float opacity{1.0F};
    bool outline{true};
    float outlineThickness{1.0F};
    bool centerDot{};
    float dotRadius{1.5F};
};

// Pure geometry: arms are pixel-snapped around the integer screen center so
// odd thicknesses stay crisp. Outline primitives precede their fills.
[[nodiscard]] OverlayPrimitives BuildCrosshair(const CrosshairStyle& style, float width, float height);
[[nodiscard]] std::uint32_t ApplyOpacity(std::uint32_t argb, float opacity) noexcept;

// ----------------------------------------------------------------------
// Presentation boundary for visual features that need a game render hook.
//
// Each module contributes its part of a PresentationProfile; a validated
// render adapter (none exists yet for the supported build - see the
// capability reasons) is the only thing that may apply it, and only to the
// local render pipeline. Every field is already clamped to the same range
// the game's own options allow, so nothing here can produce hidden
// information (no zoom beyond the vanilla FOV slider, no see-through).
// ----------------------------------------------------------------------
inline constexpr float MinimumFov = 30.0F;   // vanilla FOV slider bounds
inline constexpr float MaximumFov = 110.0F;

struct BlockOutlineRequest { std::uint32_t color{}; float thickness{}; float fillOpacity{}; };
struct HurtTintRequest { std::uint32_t color{}; float opacity{}; };
struct FovRequest { float fieldOfView{70.0F}; std::size_t profile{}; };
struct ViewBobbingRequest { float strength{1.0F}; };
struct HandViewRequest { float offsetX{}; float offsetY{}; float offsetZ{}; float scale{1.0F}; };

struct PresentationProfile {
    std::optional<BlockOutlineRequest> blockOutline;
    std::optional<HurtTintRequest> hurtTint;
    std::optional<FovRequest> fov;
    std::optional<ViewBobbingRequest> viewBobbing;
    std::optional<HandViewRequest> handView;
};

class IPresentationModule {
public:
    virtual ~IPresentationModule() = default;
    virtual void Contribute(PresentationProfile& profile) const = 0;
};

// What a render adapter would apply this frame: contributions from every
// enabled (therefore capability-validated) presentation module.
[[nodiscard]] PresentationProfile CollectPresentationProfile(const ModuleManager& modules);

// FOV profile cycling (pure): next profile index.
[[nodiscard]] std::size_t NextFovProfile(std::size_t current, std::size_t count) noexcept;

}
