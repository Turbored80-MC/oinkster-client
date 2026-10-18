#pragma once

#include "Oinkster/Module.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
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

// A crosshair the player drew on the 16x16 pixel grid. The grid's centre
// (between cells 7 and 8) sits on the pixel-snapped screen centre; each cell
// is `scale` screen pixels. Outline primitives precede their fills.
struct PixelCrosshairStyle {
    PixelGrid pixels{};
    float scale{2.0F};
    std::uint32_t color{0xFFFFFFFFU};
    float opacity{1.0F};
    bool outline{true};
    float outlineThickness{1.0F};
};
// A plus two cells thick and ten long, centred on the grid.
[[nodiscard]] PixelGrid DefaultCrosshairPixels() noexcept;
[[nodiscard]] OverlayPrimitives BuildPixelCrosshair(const PixelCrosshairStyle& style, float width, float height);

// The game's own crosshair texture redrawn at screen centre, one rect per
// run of equal pixels, `scale` screen pixels per texel. Fully transparent
// texels are skipped; images over MaxCrosshairImageEdge on a side are
// sampled down so the primitive count stays bounded.
inline constexpr std::uint32_t MaxCrosshairImageEdge = 64;
[[nodiscard]] OverlayPrimitives BuildImageCrosshair(const ArgbImage& image, float scale, float opacity, float width, float height);

// Whether Oinkster draws its own crosshair at all. Game leaves the game's
// crosshair exactly as it is: Oinkster never erases or patches it, so
// "Custom" and "Drawn" draw on top of whatever the game shows.
enum class CrosshairSource : std::uint8_t { Custom, Drawn, Game };
// Which perspectives the custom crosshair is drawn in.
enum class CrosshairPerspectiveFilter : std::uint8_t { FirstPersonOnly, AllPerspectives };

// Setting values ("custom-crosshair.source" / ".perspective"). Anything
// unrecognized - including a missing key from an older settings file -
// falls back to the first value, which is the pre-existing behavior.
inline constexpr std::string_view CrosshairSourceCustom = "custom";
inline constexpr std::string_view CrosshairSourceDrawn = "drawn";
inline constexpr std::string_view CrosshairSourceGame = "game";
inline constexpr std::string_view CrosshairPerspectiveFirstPerson = "first-person";
inline constexpr std::string_view CrosshairPerspectiveAll = "all";
[[nodiscard]] CrosshairSource ParseCrosshairSource(std::string_view value) noexcept;
[[nodiscard]] CrosshairPerspectiveFilter ParseCrosshairPerspective(std::string_view value) noexcept;

struct CrosshairConfig {
    CrosshairSource source{CrosshairSource::Custom};
    CrosshairPerspectiveFilter perspective{CrosshairPerspectiveFilter::FirstPersonOnly};
    CrosshairStyle style;
    PixelCrosshairStyle pixels;
    // Game source: redraw the game's crosshair while in third person, where
    // Bedrock hides it.
    bool gameInThirdPerson{true};
    float gameScale{2.0F};
};

// FirstPersonOnly suppresses the custom crosshair only for a *known*
// third-person perspective. Unknown (no validated perspective source - the
// case on the supported build today) draws, so the default mode stays usable
// instead of silently hiding the crosshair forever.
[[nodiscard]] bool ShouldDrawCustomCrosshair(CrosshairSource source, CrosshairPerspectiveFilter filter,
    CameraPerspective perspective) noexcept;
// Game source only: true for a *known* third-person camera. Never on
// Unknown, so the game's own first-person crosshair is never doubled.
[[nodiscard]] bool ShouldDrawGameCrosshairCopy(CrosshairSource source, bool enabled, CameraPerspective perspective) noexcept;
// The crosshair for one frame: the custom or drawn crosshair when
// ShouldDrawCustomCrosshair, the game's texture when
// ShouldDrawGameCrosshairCopy and it is known, else nothing.
[[nodiscard]] OverlayPrimitives BuildCrosshairFrame(const CrosshairConfig& config, CameraPerspective perspective,
    float width, float height, const ArgbImage* gameCrosshair = nullptr);

// ----------------------------------------------------------------------
// Presentation boundary for visual features that need a game render hook.
//
// Each module contributes its part of a PresentationProfile; a validated
// render adapter (none exists yet for the supported build - see the
// capability reasons) is the only thing that may apply it, and only to the
// local render pipeline. Every field is bounded and local-only; modules may
// deliberately exceed vanilla option ranges (for example deep zoom), but may
// not mutate packets, world state, or player physics.
// ----------------------------------------------------------------------
inline constexpr float MinimumFov = 30.0F;   // vanilla FOV slider bounds
inline constexpr float MaximumFov = 110.0F;
inline constexpr float MinimumZoomFov = 5.0F;

struct BlockOutlineRequest { std::uint32_t color{}; float thickness{}; float fillOpacity{}; };
struct HurtTintRequest { std::uint32_t color{}; float opacity{}; };
struct FovRequest { float fieldOfView{70.0F}; std::size_t profile{}; };
struct ZoomRequest { float fieldOfView{30.0F}; float transitionSeconds{0.12F}; };
struct CameraRotationRequest { bool active{}; float sensitivity{1.0F}; float returnSeconds{0.15F}; };
struct CinematicCameraRequest { float strength{0.75F}; float transitionSeconds{0.12F}; };
struct ViewBobbingRequest { float strength{1.0F}; };
struct HurtCameraRequest { float strength{}; };
struct ThirdPersonDistanceRequest { float distance{4.0F}; bool front{true}; bool back{true}; };
struct HandViewRequest { float offsetX{}; float offsetY{}; float offsetZ{}; float scale{1.0F}; };
struct LightingRequest { float multiplier{5.0F}; };
struct TimeRequest { int ticks{6000}; };
struct WeatherRequest { float rain{}; float snow{}; float thunder{}; };
struct ScreenEffectsRequest {
    float fire{};
    float powderSnow{};
    float portal{};
    float pumpkin{};
    float vignette{};
    float nausea{};
};

// Snap look: the perspective the game should render from while held.
struct PerspectiveRequest { CameraPerspective perspective{CameraPerspective::ThirdPersonFront}; };
// Asks the adapter to publish CameraPoseEvent every frame (and, with
// `selection`, BlockSelectionEvent too); draws nothing itself.
struct CameraPoseRequest { bool selection{}; };

struct PresentationProfile {
    std::optional<BlockOutlineRequest> blockOutline;
    std::optional<PerspectiveRequest> perspective;
    std::optional<CameraPoseRequest> cameraPose;
    std::optional<HurtTintRequest> hurtTint;
    std::optional<FovRequest> fov;
    std::optional<ZoomRequest> zoom;
    std::optional<CameraRotationRequest> cameraRotation;
    std::optional<CinematicCameraRequest> cinematicCamera;
    std::optional<ViewBobbingRequest> viewBobbing;
    std::optional<HurtCameraRequest> hurtCamera;
    std::optional<ThirdPersonDistanceRequest> thirdPersonDistance;
    std::optional<HandViewRequest> handView;
    std::optional<LightingRequest> lighting;
    std::optional<TimeRequest> time;
    std::optional<WeatherRequest> weather;
    std::optional<ScreenEffectsRequest> screenEffects;
};

class IPresentationModule {
public:
    virtual ~IPresentationModule() = default;
    virtual void Contribute(PresentationProfile& profile) const = 0;
};

// What a render adapter would apply this frame: contributions from every
// enabled (therefore capability-validated) presentation module.
[[nodiscard]] PresentationProfile CollectPresentationProfile(const ModuleManager& modules);

// Zoom has deterministic precedence over the selected FOV profile. The
// adapter supplies vanillaFov when neither module contributes.
[[nodiscard]] float EffectiveFov(const PresentationProfile& profile, float vanillaFov) noexcept;

// Distance overrides are valid only for a positively identified third-person
// perspective. Unknown and first-person views preserve the game's value.
[[nodiscard]] float EffectiveThirdPersonDistance(const PresentationProfile& profile,
    CameraPerspective perspective, float vanillaDistance) noexcept;

// The selected block's outline as screen geometry: the edges of every face
// that faces the camera (so the far edges stay hidden, as in vanilla), and
// the looked-at face filled when fillOpacity > 0. Projected with the event's
// camera; corners behind the camera are dropped with their edges.
[[nodiscard]] OverlayPrimitives BuildBlockOutline(const BlockOutlineRequest& style, const BlockSelectionEvent& selection,
    float width, float height);

// A camera to project world points with: CameraPoseEvent fields in the
// form the geometry builders share.
struct CameraView {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
    float verticalFov{};
};
[[nodiscard]] CameraView ViewOf(const CameraPoseEvent& pose) noexcept;

// Projects a world-space segment, clipped against the near plane; nothing
// when it lies wholly behind the camera or the view is implausible.
[[nodiscard]] std::optional<OverlayLine> ProjectSegment(const CameraView& view, const std::array<float, 3>& from,
    const std::array<float, 3>& to, float width, float height, std::uint32_t argb, float thickness) noexcept;

struct ChunkBorderStyle {
    std::uint32_t chunkColor{0xFFFFD54AU};     // current chunk walls
    std::uint32_t cornerColor{0xFFFF4A4AU};    // current chunk corners
    std::uint32_t neighborColor{0xFF4AA8FFU};  // neighbouring chunk corners
    float thickness{1.5F};
    int gridSpacing{2};                        // blocks between wall lines, 0 = corners only
    int verticalRange{24};                     // blocks above and below the player
};

// Java-style chunk borders (F3+G) around the chunk containing `player`:
// the four walls of the current chunk as a grid, its corners, and the
// corners of the neighbouring chunks. Drawn over the frame (no depth test).
[[nodiscard]] OverlayPrimitives BuildChunkBorders(const ChunkBorderStyle& style, const CameraView& view,
    const std::array<float, 3>& player, float width, float height);

// FOV profile cycling (pure): next profile index.
[[nodiscard]] std::size_t NextFovProfile(std::size_t current, std::size_t count) noexcept;

}
