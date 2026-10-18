#include "Oinkster/Modules/Factories.hpp"
#include "Oinkster/Modules/VisualLogic.hpp"
#include "Oinkster/Modules/CameraMath.hpp"
#include "Oinkster/ModuleManager.hpp"
#include <algorithm>
#include <cmath>
#include <format>

namespace Oinkster::Modules {

std::uint32_t ApplyOpacity(std::uint32_t argb, float opacity) noexcept {
    const float clamped = std::isfinite(opacity) ? std::clamp(opacity, 0.0F, 1.0F) : 1.0F;
    const auto alpha = static_cast<std::uint32_t>(std::lround(static_cast<float>((argb >> 24U) & 0xFFU) * clamped));
    return (argb & 0x00FFFFFFU) | (alpha << 24U);
}

OverlayPrimitives BuildCrosshair(const CrosshairStyle& style, float width, float height) {
    OverlayPrimitives primitives;
    if (!(width > 0.0F) || !(height > 0.0F)) return primitives;
    const float cx = std::floor(width * 0.5F);
    const float cy = std::floor(height * 0.5F);
    const float thickness = std::clamp(style.thickness, 1.0F, 8.0F);
    const float gap = std::clamp(style.gap, 0.0F, 32.0F);
    const float length = std::clamp(style.length, 0.0F, 48.0F);
    const float half = thickness * 0.5F;
    const auto fill = ApplyOpacity(style.color, style.opacity);
    const auto outlineColor = ApplyOpacity(0xFF000000U, std::clamp(style.opacity, 0.0F, 1.0F) * 0.85F);
    const float border = std::clamp(style.outlineThickness, 0.5F, 3.0F);

    std::vector<OverlayRect> arms;
    if (length > 0.0F) {
        arms.push_back({cx - gap - length, cy - half, cx - gap, cy + half, fill, true, 1.0F});
        arms.push_back({cx + gap, cy - half, cx + gap + length, cy + half, fill, true, 1.0F});
        arms.push_back({cx - half, cy - gap - length, cx + half, cy - gap, fill, true, 1.0F});
        arms.push_back({cx - half, cy + gap, cx + half, cy + gap + length, fill, true, 1.0F});
    }
    if (style.outline) {
        for (const auto& arm : arms) {
            primitives.rects.push_back({arm.x1 - border, arm.y1 - border, arm.x2 + border, arm.y2 + border, outlineColor, true, 1.0F});
        }
    }
    primitives.rects.insert(primitives.rects.end(), arms.begin(), arms.end());
    if (style.centerDot) {
        const float radius = std::clamp(style.dotRadius, 0.5F, 6.0F);
        if (style.outline) primitives.circles.push_back({cx, cy, radius + border, outlineColor, true, 1.0F});
        primitives.circles.push_back({cx, cy, radius, fill, true, 1.0F});
    }
    return primitives;
}

PixelGrid DefaultCrosshairPixels() noexcept {
    PixelGrid grid{};
    for (std::size_t index = 3; index <= 12; ++index) {
        for (const std::size_t middle : {std::size_t{7}, std::size_t{8}}) {
            SetPixel(grid, index, middle, true);
            SetPixel(grid, middle, index, true);
        }
    }
    return grid;
}

OverlayPrimitives BuildPixelCrosshair(const PixelCrosshairStyle& style, float width, float height) {
    OverlayPrimitives primitives;
    if (!(width > 0.0F) || !(height > 0.0F)) return primitives;
    const float scale = std::isfinite(style.scale) ? std::clamp(style.scale, 1.0F, 8.0F) : 2.0F;
    const float half = static_cast<float>(PixelGridSize) * 0.5F * scale;
    const float left = std::floor(width * 0.5F) - half;
    const float top = std::floor(height * 0.5F) - half;
    const auto fill = ApplyOpacity(style.color, style.opacity);
    const auto outlineColor = ApplyOpacity(0xFF000000U, std::clamp(style.opacity, 0.0F, 1.0F) * 0.85F);
    const float border = std::clamp(style.outlineThickness, 0.5F, 3.0F);

    std::vector<OverlayRect> runs;
    for (std::size_t row = 0; row < PixelGridSize; ++row) {
        std::size_t column = 0;
        while (column < PixelGridSize) {
            if (!PixelAt(style.pixels, column, row)) { ++column; continue; }
            const std::size_t start = column;
            while (column < PixelGridSize && PixelAt(style.pixels, column, row)) ++column;
            runs.push_back({left + static_cast<float>(start) * scale, top + static_cast<float>(row) * scale,
                left + static_cast<float>(column) * scale, top + static_cast<float>(row + 1) * scale, fill, true, 1.0F});
        }
    }
    if (style.outline) {
        for (const auto& run : runs) {
            primitives.rects.push_back({run.x1 - border, run.y1 - border, run.x2 + border, run.y2 + border, outlineColor, true, 1.0F});
        }
    }
    primitives.rects.insert(primitives.rects.end(), runs.begin(), runs.end());
    return primitives;
}

OverlayPrimitives BuildImageCrosshair(const ArgbImage& image, float scale, float opacity, float width, float height) {
    OverlayPrimitives primitives;
    if (!image.Valid() || !(width > 0.0F) || !(height > 0.0F)) return primitives;
    const std::uint32_t step = (std::max(image.width, image.height) + MaxCrosshairImageEdge - 1) / MaxCrosshairImageEdge;
    const std::uint32_t columns = (image.width + step - 1) / step;
    const std::uint32_t rows = (image.height + step - 1) / step;
    const float cell = (std::isfinite(scale) ? std::clamp(scale, 0.5F, 8.0F) : 2.0F) * static_cast<float>(step);
    const float left = std::floor(width * 0.5F - static_cast<float>(columns) * cell * 0.5F);
    const float top = std::floor(height * 0.5F - static_cast<float>(rows) * cell * 0.5F);
    const auto texel = [&](std::uint32_t column, std::uint32_t row) {
        return image.pixels[static_cast<std::size_t>(row) * step * image.width + static_cast<std::size_t>(column) * step];
    };
    for (std::uint32_t row = 0; row < rows; ++row) {
        std::uint32_t column = 0;
        while (column < columns) {
            const auto argb = texel(column, row);
            const std::uint32_t start = column;
            while (column < columns && texel(column, row) == argb) ++column;
            if ((argb >> 24U) == 0) continue;
            primitives.rects.push_back({left + static_cast<float>(start) * cell, top + static_cast<float>(row) * cell,
                left + static_cast<float>(column) * cell, top + static_cast<float>(row + 1) * cell, ApplyOpacity(argb, opacity), true, 1.0F});
        }
    }
    return primitives;
}

CrosshairSource ParseCrosshairSource(std::string_view value) noexcept {
    if (value == CrosshairSourceGame) return CrosshairSource::Game;
    return value == CrosshairSourceDrawn ? CrosshairSource::Drawn : CrosshairSource::Custom;
}

CrosshairPerspectiveFilter ParseCrosshairPerspective(std::string_view value) noexcept {
    return value == CrosshairPerspectiveAll ? CrosshairPerspectiveFilter::AllPerspectives : CrosshairPerspectiveFilter::FirstPersonOnly;
}

bool ShouldDrawCustomCrosshair(CrosshairSource source, CrosshairPerspectiveFilter filter, CameraPerspective perspective) noexcept {
    if (source == CrosshairSource::Game) return false;
    if (filter == CrosshairPerspectiveFilter::AllPerspectives) return true;
    return perspective == CameraPerspective::FirstPerson || perspective == CameraPerspective::Unknown;
}

bool ShouldDrawGameCrosshairCopy(CrosshairSource source, bool enabled, CameraPerspective perspective) noexcept {
    return source == CrosshairSource::Game && enabled
        && (perspective == CameraPerspective::ThirdPersonBack || perspective == CameraPerspective::ThirdPersonFront);
}

OverlayPrimitives BuildCrosshairFrame(const CrosshairConfig& config, CameraPerspective perspective, float width, float height,
    const ArgbImage* gameCrosshair) {
    if (ShouldDrawGameCrosshairCopy(config.source, config.gameInThirdPerson, perspective)) {
        return gameCrosshair ? BuildImageCrosshair(*gameCrosshair, config.gameScale, 1.0F, width, height) : OverlayPrimitives{};
    }
    if (!ShouldDrawCustomCrosshair(config.source, config.perspective, perspective)) return {};
    if (config.source == CrosshairSource::Drawn) return BuildPixelCrosshair(config.pixels, width, height);
    return BuildCrosshair(config.style, width, height);
}

PresentationProfile CollectPresentationProfile(const ModuleManager& modules) {
    PresentationProfile profile;
    for (const auto* module : modules.EnabledModulesOf<IPresentationModule>()) module->Contribute(profile);
    return profile;
}

float EffectiveFov(const PresentationProfile& profile, float vanillaFov) noexcept {
    if (profile.zoom) return std::clamp(profile.zoom->fieldOfView, MinimumZoomFov, MaximumFov);
    if (profile.fov) return std::clamp(profile.fov->fieldOfView, MinimumFov, MaximumFov);
    return std::clamp(vanillaFov, MinimumZoomFov, MaximumFov);
}

float EffectiveThirdPersonDistance(const PresentationProfile& profile, CameraPerspective perspective,
    float vanillaDistance) noexcept {
    if (!profile.thirdPersonDistance) return vanillaDistance;
    const auto& request = *profile.thirdPersonDistance;
    const bool applies = (perspective == CameraPerspective::ThirdPersonBack && request.back)
        || (perspective == CameraPerspective::ThirdPersonFront && request.front);
    return applies ? std::clamp(request.distance, 1.0F, 20.0F) : vanillaDistance;
}

OverlayPrimitives BuildBlockOutline(const BlockOutlineRequest& style, const BlockSelectionEvent& selection, float width, float height) {
    namespace CM = CameraMath;
    OverlayPrimitives primitives;
    if (!selection.hasBlock || width <= 0.0F || height <= 0.0F || !(selection.verticalFov > 0.01F && selection.verticalFov < 3.1F)) {
        return primitives;
    }
    const CM::Quat rotation{selection.cameraRotation[0], selection.cameraRotation[1], selection.cameraRotation[2], selection.cameraRotation[3]};
    const CM::Quat toCamera{-rotation.x, -rotation.y, -rotation.z, rotation.w};
    const CM::Float3 eye{selection.cameraPosition[0], selection.cameraPosition[1], selection.cameraPosition[2]};
    const float tanHalf = std::tan(selection.verticalFov * 0.5F);
    const float aspect = width / height;
    // Slightly larger than the block so the lines sit on its surface.
    constexpr float Grow = 0.002F;
    const CM::Float3 low{static_cast<float>(selection.block[0]) - Grow, static_cast<float>(selection.block[1]) - Grow,
        static_cast<float>(selection.block[2]) - Grow};
    const CM::Float3 high = low + CM::Float3{1.0F + 2.0F * Grow, 1.0F + 2.0F * Grow, 1.0F + 2.0F * Grow};

    // Corner bit 0 = x, bit 1 = y, bit 2 = z (set = high side).
    std::array<std::optional<std::pair<float, float>>, 8> screen{};
    for (std::size_t corner = 0; corner < 8; ++corner) {
        const CM::Float3 world{(corner & 1U) ? high.x : low.x, (corner & 2U) ? high.y : low.y, (corner & 4U) ? high.z : low.z};
        const CM::Float3 view = CM::Rotate(toCamera, world - eye);
        if (-view.z < 0.05F) continue;
        const float x = (view.x / -view.z) / (tanHalf * aspect);
        const float y = (view.y / -view.z) / tanHalf;
        screen[corner] = std::pair{(x + 1.0F) * 0.5F * width, (1.0F - y) * 0.5F * height};
    }

    // Faces in BlockFace order: down, up, north (-z), south (+z), west (-x), east (+x).
    struct Face { CM::Float3 normal; std::array<std::size_t, 4> corners; };
    const std::array<Face, 6> faces{{
        {{0.0F, -1.0F, 0.0F}, {0, 1, 5, 4}}, {{0.0F, 1.0F, 0.0F}, {2, 3, 7, 6}},
        {{0.0F, 0.0F, -1.0F}, {0, 1, 3, 2}}, {{0.0F, 0.0F, 1.0F}, {4, 5, 7, 6}},
        {{-1.0F, 0.0F, 0.0F}, {0, 2, 6, 4}}, {{1.0F, 0.0F, 0.0F}, {1, 3, 7, 5}}}};
    const CM::Float3 center = (low + high) * 0.5F;
    std::array<bool, 6> facing{};
    for (std::size_t index = 0; index < faces.size(); ++index) {
        const CM::Float3 faceCenter = center + faces[index].normal * 0.5F;
        facing[index] = CM::Dot(faces[index].normal, eye - faceCenter) > 0.0F;
    }

    const auto faceIndex = static_cast<int>(selection.face);
    if (style.fillOpacity > 0.0F && faceIndex >= 0 && faceIndex < 6 && facing[static_cast<std::size_t>(faceIndex)]) {
        const auto& face = faces[static_cast<std::size_t>(faceIndex)];
        OverlayQuad quad;
        quad.argb = ApplyOpacity(style.color, std::clamp(style.fillOpacity, 0.0F, 1.0F));
        bool visible = true;
        for (std::size_t corner = 0; corner < 4; ++corner) {
            const auto& point = screen[face.corners[corner]];
            if (!point) { visible = false; break; }
            quad.x[corner] = point->first;
            quad.y[corner] = point->second;
        }
        if (visible) primitives.quads.push_back(quad);
    }

    // Each edge with the two faces it borders.
    struct Edge { std::size_t a, b, faceA, faceB; };
    std::array<Edge, 12> edges{};
    std::size_t count = 0;
    for (std::size_t first = 0; first < 2; ++first) {
        for (std::size_t second = 0; second < 2; ++second) {
            // Along x (y = first, z = second), along y (x = first, z = second), along z (x = first, y = second).
            edges[count++] = {(first << 1U) | (second << 2U), (first << 1U) | (second << 2U) | 1U, first ? 1U : 0U, second ? 3U : 2U};
            edges[count++] = {first | (second << 2U), first | (second << 2U) | 2U, first ? 5U : 4U, second ? 3U : 2U};
            edges[count++] = {first | (second << 1U), first | (second << 1U) | 4U, first ? 5U : 4U, second ? 1U : 0U};
        }
    }
    const float thickness = std::clamp(style.thickness, 1.0F, 5.0F);
    for (const auto& edge : edges) {
        if (!facing[edge.faceA] && !facing[edge.faceB]) continue;
        const auto& from = screen[edge.a];
        const auto& to = screen[edge.b];
        if (!from || !to) continue;
        primitives.lines.push_back({from->first, from->second, to->first, to->second, style.color, thickness});
    }
    return primitives;
}

CameraView ViewOf(const CameraPoseEvent& pose) noexcept {
    return {pose.position, pose.rotation, pose.verticalFov};
}

std::optional<OverlayLine> ProjectSegment(const CameraView& view, const std::array<float, 3>& from,
    const std::array<float, 3>& to, float width, float height, std::uint32_t argb, float thickness) noexcept {
    namespace CM = CameraMath;
    if (width <= 0.0F || height <= 0.0F || !(view.verticalFov > 0.01F && view.verticalFov < 3.1F)) return std::nullopt;
    const CM::Quat rotation{view.rotation[0], view.rotation[1], view.rotation[2], view.rotation[3]};
    if (!CM::IsFinite(rotation)) return std::nullopt;
    const CM::Quat toCamera{-rotation.x, -rotation.y, -rotation.z, rotation.w};
    const CM::Float3 eye{view.position[0], view.position[1], view.position[2]};
    CM::Float3 a = CM::Rotate(toCamera, CM::Float3{from[0], from[1], from[2]} - eye);
    CM::Float3 b = CM::Rotate(toCamera, CM::Float3{to[0], to[1], to[2]} - eye);
    // Depth is -z; clip whatever lies nearer than the near plane.
    constexpr float Near = 0.05F;
    const float depthA = -a.z;
    const float depthB = -b.z;
    if (depthA < Near && depthB < Near) return std::nullopt;
    if (depthA < Near) a = a + (b - a) * ((Near - depthA) / (depthB - depthA));
    else if (depthB < Near) b = b + (a - b) * ((Near - depthB) / (depthA - depthB));
    const float tanHalf = std::tan(view.verticalFov * 0.5F);
    const float aspect = width / height;
    const auto screen = [&](CM::Float3 point) {
        const float x = (point.x / -point.z) / (tanHalf * aspect);
        const float y = (point.y / -point.z) / tanHalf;
        return std::pair{(x + 1.0F) * 0.5F * width, (1.0F - y) * 0.5F * height};
    };
    const auto [x1, y1] = screen(a);
    const auto [x2, y2] = screen(b);
    if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2)) return std::nullopt;
    float clippedX1 = x1;
    float clippedY1 = y1;
    float clippedX2 = x2;
    float clippedY2 = y2;
    const float dx = clippedX2 - clippedX1;
    const float dy = clippedY2 - clippedY1;
    float enter = 0.0F;
    float leave = 1.0F;
    const auto clip = [&](float p, float q) {
        if (std::abs(p) < 1e-6F) return q >= 0.0F;
        const float ratio = q / p;
        if (p < 0.0F) {
            if (ratio > leave) return false;
            enter = std::max(enter, ratio);
        } else {
            if (ratio < enter) return false;
            leave = std::min(leave, ratio);
        }
        return true;
    };
    if (!clip(-dx, clippedX1) || !clip(dx, width - clippedX1)
        || !clip(-dy, clippedY1) || !clip(dy, height - clippedY1)) return std::nullopt;
    clippedX2 = clippedX1 + leave * dx;
    clippedY2 = clippedY1 + leave * dy;
    clippedX1 += enter * dx;
    clippedY1 += enter * dy;
    return OverlayLine{clippedX1, clippedY1, clippedX2, clippedY2, argb, thickness};
}

OverlayPrimitives BuildChunkBorders(const ChunkBorderStyle& style, const CameraView& view,
    const std::array<float, 3>& player, float width, float height) {
    OverlayPrimitives primitives;
    if (!std::isfinite(player[0]) || !std::isfinite(player[1]) || !std::isfinite(player[2])) return primitives;
    const float thickness = std::clamp(style.thickness, 0.5F, 5.0F);
    const int range = std::clamp(style.verticalRange, 4, 128);
    const auto chunkX = static_cast<int>(std::floor(player[0] / 16.0F));
    const auto chunkZ = static_cast<int>(std::floor(player[2] / 16.0F));
    const auto baseY = static_cast<int>(std::floor(player[1]));
    const auto bottom = static_cast<float>(baseY - range);
    const auto top = static_cast<float>(baseY + range);
    const auto x0 = static_cast<float>(chunkX * 16);
    const auto z0 = static_cast<float>(chunkZ * 16);
    const auto add = [&](std::array<float, 3> from, std::array<float, 3> to, std::uint32_t color,
        float opacity, float thicknessScale) {
        const float middleX = (from[0] + to[0]) * 0.5F;
        const float middleZ = (from[2] + to[2]) * 0.5F;
        const float distance = std::hypot(middleX - player[0], middleZ - player[2]);
        const float distanceFade = std::clamp(1.0F - (distance - 12.0F) / 36.0F, 0.18F, 1.0F);
        if (auto line = ProjectSegment(view, from, to, width, height, ApplyOpacity(color, opacity * distanceFade),
                thickness * thicknessScale)) {
            primitives.lines.push_back(*line);
        }
    };
    // Neighbouring chunks' corners: the 4x4 lattice around the current
    // chunk without its own four corners.
    for (int i = -1; i <= 2; ++i) {
        for (int j = -1; j <= 2; ++j) {
            if ((i == 0 || i == 1) && (j == 0 || j == 1)) continue;
            const float x = x0 + static_cast<float>(i * 16);
            const float z = z0 + static_cast<float>(j * 16);
            add({x, bottom, z}, {x, top, z}, style.neighborColor, 0.38F, 0.70F);
        }
    }
    const int spacing = std::clamp(style.gridSpacing, 0, 16);
    if (spacing > 0) {
        for (int step = spacing; step < 16; step += spacing) {
            const auto offset = static_cast<float>(step);
            add({x0 + offset, bottom, z0}, {x0 + offset, top, z0}, style.chunkColor, 0.48F, 0.62F);
            add({x0 + offset, bottom, z0 + 16.0F}, {x0 + offset, top, z0 + 16.0F}, style.chunkColor, 0.48F, 0.62F);
            add({x0, bottom, z0 + offset}, {x0, top, z0 + offset}, style.chunkColor, 0.48F, 0.62F);
            add({x0 + 16.0F, bottom, z0 + offset}, {x0 + 16.0F, top, z0 + offset}, style.chunkColor, 0.48F, 0.62F);
        }
        for (int y = baseY - range; y <= baseY + range; y += spacing) {
            const auto level = static_cast<float>(y);
            const bool section = (y % 16 + 16) % 16 == 0;
            const float opacity = section ? 0.88F : 0.42F;
            const float weight = section ? 1.15F : 0.58F;
            add({x0, level, z0}, {x0 + 16.0F, level, z0}, style.chunkColor, opacity, weight);
            add({x0, level, z0 + 16.0F}, {x0 + 16.0F, level, z0 + 16.0F}, style.chunkColor, opacity, weight);
            add({x0, level, z0}, {x0, level, z0 + 16.0F}, style.chunkColor, opacity, weight);
            add({x0 + 16.0F, level, z0}, {x0 + 16.0F, level, z0 + 16.0F}, style.chunkColor, opacity, weight);
        }
    }
    // The current chunk's corners last, so they draw over the grid.
    for (int i = 0; i <= 1; ++i) {
        for (int j = 0; j <= 1; ++j) {
            const float x = x0 + static_cast<float>(i * 16);
            const float z = z0 + static_cast<float>(j * 16);
            add({x, bottom, z}, {x, top, z}, style.cornerColor, 1.0F, 1.55F);
        }
    }
    return primitives;
}

std::size_t NextFovProfile(std::size_t current, std::size_t count) noexcept {
    return count == 0 ? 0 : (current + 1) % count;
}

namespace {
// Visual-only modules that draw nothing in the HUD.
template<typename... Mixins>
class VisualModule : public ModuleBase<IModule>, public Mixins... {
public:
    using ModuleBase<IModule>::ModuleBase;
};

class CustomCrosshairModule final : public VisualModule<IOverlayModule> {
public:
    CustomCrosshairModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            ChoiceSetting("source", "Crosshair source", std::string(CrosshairSourceCustom), {
                {std::string(CrosshairSourceCustom), "Custom"},
                {std::string(CrosshairSourceDrawn), "Drawn"},
                {std::string(CrosshairSourceGame), "Game default"}},
                "Drawn uses the pixel grid below. Game default keeps the game's own crosshair (and your "
                "texture pack's) and never draws over it in first person."),
            ShownWhen(BoolSetting("third-person", "Show in 3rd person", true,
                "Bedrock hides its crosshair in 3rd person; this redraws the one your resource packs use there."),
                "source", {std::string(CrosshairSourceGame)}),
            ShownWhen(FloatSetting("game-scale", "Size", 2.0, 1.0, 8.0, "%.1fx"), "source", {std::string(CrosshairSourceGame)}),
            ShownWhen(ChoiceSetting("perspective", "Show custom crosshair", std::string(CrosshairPerspectiveFirstPerson), {
                {std::string(CrosshairPerspectiveFirstPerson), "First person only"},
                {std::string(CrosshairPerspectiveAll), "All perspectives"}},
                "Until the game's camera mode has been read, first person only also draws in 3rd person."),
                "source", {std::string(CrosshairSourceCustom), std::string(CrosshairSourceDrawn)}),
            ShownWhen(PixelSetting("pixels", "Draw", DefaultCrosshairPixels(), "Left-click paints, right-click erases."),
                "source", {std::string(CrosshairSourceDrawn)}),
            ShownWhen(FloatSetting("pixel-scale", "Pixel size", 2.0, 1.0, 8.0, "%.1f px"), "source", {std::string(CrosshairSourceDrawn)}),
            ShownWhen(FloatSetting("size", "Arm length", 6.0, 0.0, 24.0, "%.0f px"), "source", {std::string(CrosshairSourceCustom)}),
            ShownWhen(FloatSetting("gap", "Gap", 3.0, 0.0, 16.0, "%.0f px"), "source", {std::string(CrosshairSourceCustom)}),
            ShownWhen(FloatSetting("thickness", "Thickness", 2.0, 1.0, 6.0, "%.1f px"), "source", {std::string(CrosshairSourceCustom)}),
            ShownWhen(ColorSetting("color", "Color", 0xFFFFFFFFU), "source", OwnStyleSources()),
            ShownWhen(FloatSetting("opacity", "Opacity", 1.0, 0.1, 1.0, "%.2f"), "source", OwnStyleSources()),
            ShownWhen(BoolSetting("outline", "Outline", true), "source", OwnStyleSources()),
            ShownWhen(FloatSetting("outline-thickness", "Outline thickness", 1.0, 0.5, 3.0, "%.1f px"), "source", OwnStyleSources()),
            ShownWhen(BoolSetting("center-dot", "Center dot", false), "source", {std::string(CrosshairSourceCustom)}),
            ShownWhen(FloatSetting("dot-size", "Dot radius", 1.5, 0.5, 6.0, "%.1f px"), "source", {std::string(CrosshairSourceCustom)})}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        CrosshairConfig config;
        config.source = ParseCrosshairSource(settings.Choice("source"));
        config.perspective = ParseCrosshairPerspective(settings.Choice("perspective"));
        config.gameInThirdPerson = settings.Bool("third-person");
        config.gameScale = settings.Float("game-scale");
        auto& style = config.style;
        style.length = settings.Float("size");
        style.gap = settings.Float("gap");
        style.thickness = settings.Float("thickness");
        style.color = settings.Color("color");
        style.opacity = settings.Float("opacity");
        style.outline = settings.Bool("outline");
        style.outlineThickness = settings.Float("outline-thickness");
        style.centerDot = settings.Bool("center-dot");
        style.dotRadius = settings.Float("dot-size");
        auto& pixels = config.pixels;
        pixels.pixels = settings.Pixels("pixels");
        pixels.scale = settings.Float("pixel-scale");
        pixels.color = style.color;
        pixels.opacity = style.opacity;
        pixels.outline = style.outline;
        pixels.outlineThickness = style.outlineThickness;
        std::scoped_lock lock(mutex_);
        config_ = config;
    }
    // A perspective or texture seen before the module was last disabled is stale.
    void OnEnable() override {
        std::scoped_lock lock(mutex_);
        perspective_ = CameraPerspective::Unknown;
        gameCrosshair_.reset();
    }
    void OnCameraPerspective(const CameraPerspectiveEvent& event) override {
        std::scoped_lock lock(mutex_);
        perspective_ = event.perspective;
    }
    void OnGameCrosshair(const GameCrosshairEvent& event) override {
        std::scoped_lock lock(mutex_);
        gameCrosshair_ = event.image;
    }
    [[nodiscard]] OverlayPrimitives Primitives(float width, float height) const override {
        CrosshairConfig config;
        CameraPerspective perspective{};
        std::shared_ptr<const ArgbImage> gameCrosshair;
        {
            std::scoped_lock lock(mutex_);
            config = config_;
            perspective = perspective_;
            gameCrosshair = gameCrosshair_;
        }
        return BuildCrosshairFrame(config, perspective, width, height, gameCrosshair.get());
    }

private:
    // Sources that draw Oinkster's own crosshair, so its color/outline apply.
    static std::vector<std::string> OwnStyleSources() {
        return {std::string(CrosshairSourceCustom), std::string(CrosshairSourceDrawn)};
    }

    CrosshairConfig config_;
    CameraPerspective perspective_{CameraPerspective::Unknown};
    std::shared_ptr<const ArgbImage> gameCrosshair_;
};

class BlockOutlineModule final : public VisualModule<IPresentationModule, IOverlayModule> {
public:
    BlockOutlineModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            ColorSetting("color", "Outline color", 0xFFF28FB5U),
            FloatSetting("thickness", "Thickness", 2.0, 1.0, 5.0, "%.1f px"),
            FloatSetting("fill-opacity", "Face fill opacity", 0.0, 0.0, 0.4, "%.2f",
                "Cosmetic tint on the selected face only; never reveals blocks behind others.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_ = {settings.Color("color"), settings.Float("thickness"), settings.Float("fill-opacity")};
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.blockOutline = request_; }
    void OnBlockSelection(const BlockSelectionEvent& event) override { std::scoped_lock lock(mutex_); selection_ = event; }
    void OnDisable() override { std::scoped_lock lock(mutex_); selection_ = {}; }
    [[nodiscard]] OverlayPrimitives Primitives(float width, float height) const override {
        std::scoped_lock lock(mutex_);
        // A selection older than a few frames means the level stopped rendering.
        if (SteadyClock::now() - selection_.capturedAt > std::chrono::milliseconds(250)) return {};
        return BuildBlockOutline(request_, selection_, width, height);
    }

private:
    BlockOutlineRequest request_{0xFFF28FB5U, 2.0F, 0.0F};
    BlockSelectionEvent selection_{};
};

class HurtColorModule final : public VisualModule<IPresentationModule> {
public:
    HurtColorModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            ColorSetting("color", "Tint color", 0xFFFF3030U),
            FloatSetting("opacity", "Tint strength", 0.6, 0.0, 1.0, "%.2f", "0 removes the tint for accessibility.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_ = {settings.Color("color"), settings.Float("opacity")};
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.hurtTint = request_; }

private:
    HurtTintRequest request_{0xFFFF3030U, 0.6F};
};

class MotionBlurModule final : public VisualModule<IFrameBlendModule> {
public:
    MotionBlurModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("strength", "Blend strength", 0.35, 0.05, MaxFrameBlendStrength, "%.2f",
                "Weight of the previous frame blended over the current one.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        strength_ = std::clamp(settings.Float("strength"), 0.0F, MaxFrameBlendStrength);
    }
    [[nodiscard]] float BlendStrength() const noexcept override { std::scoped_lock lock(mutex_); return strength_; }

private:
    float strength_{0.35F};
};

class FovProfilesModule final : public VisualModule<IPresentationModule> {
public:
    static constexpr std::size_t ProfileCount = 3;
    FovProfilesModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("profile-1", "Profile 1 FOV", 70.0, MinimumFov, MaximumFov, "%.0f"),
            FloatSetting("profile-2", "Profile 2 FOV", 90.0, MinimumFov, MaximumFov, "%.0f"),
            FloatSetting("profile-3", "Profile 3 FOV", 100.0, MinimumFov, MaximumFov, "%.0f"),
            IntSetting("active", "Active profile", 1, 1, static_cast<int>(ProfileCount)),
            KeySetting("cycle-key", "Cycle profile key", 0, "Switches presets; the range matches the vanilla FOV slider.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        profiles_ = {settings.Float("profile-1"), settings.Float("profile-2"), settings.Float("profile-3")};
        active_ = static_cast<std::size_t>(settings.Int("active") - 1);
        cycleKey_ = settings.Key("cycle-key");
    }
    void OnKeyboard(const KeyboardEvent& event) override {
        if (event.uiCaptured || !event.pressed || event.repeated) return;
        std::scoped_lock lock(mutex_);
        if (cycleKey_ != 0 && event.key == cycleKey_) active_ = NextFovProfile(active_, ProfileCount);
    }
    [[nodiscard]] std::vector<ModuleAction> Actions() const override { return {{"cycle", "Next profile"}}; }
    bool InvokeAction(std::string_view action, SteadyTime) override {
        if (action != "cycle") return false;
        std::scoped_lock lock(mutex_);
        active_ = NextFovProfile(active_, ProfileCount);
        return true;
    }
    void Contribute(PresentationProfile& profile) const override {
        std::scoped_lock lock(mutex_);
        const auto index = std::min(active_, ProfileCount - 1);
        profile.fov = FovRequest{std::clamp(profiles_[index], MinimumFov, MaximumFov), index};
    }

private:
    std::array<float, ProfileCount> profiles_{70.0F, 90.0F, 100.0F};
    std::size_t active_{};
    std::uint32_t cycleKey_{};
};

class ViewBobbingModule final : public VisualModule<IPresentationModule> {
public:
    ViewBobbingModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("strength", "Bobbing strength", 0.0, 0.0, 1.0, "%.2f",
                "0 removes bobbing. Partial strengths are not supported yet; above 0 keeps vanilla bobbing.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_.strength = std::clamp(settings.Float("strength"), 0.0F, 1.0F);
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.viewBobbing = request_; }

private:
    ViewBobbingRequest request_{0.0F};
};

class HandViewModule final : public VisualModule<IPresentationModule> {
public:
    HandViewModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("offset-x", "Horizontal offset", 0.0, -0.5, 0.5, "%.2f"),
            FloatSetting("offset-y", "Vertical offset", 0.0, -0.5, 0.5, "%.2f"),
            FloatSetting("offset-z", "Depth offset", 0.0, -0.5, 0.5, "%.2f"),
            FloatSetting("scale", "Scale", 1.0, 0.5, 1.5, "%.2fx")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_ = {settings.Float("offset-x"), settings.Float("offset-y"), settings.Float("offset-z"), settings.Float("scale")};
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.handView = request_; }

private:
    HandViewRequest request_{};
};

class ZoomModule final : public VisualModule<IPresentationModule> {
public:
    ZoomModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("fov", "Zoom FOV", 30.0, MinimumZoomFov, 70.0, "%.0f"),
            ChoiceSetting("mode", "Activation", "hold", {{"hold", "Hold"}, {"toggle", "Toggle"}}),
            KeySetting("key", "Activation key", 'C'),
            IntSetting("transition-ms", "Transition", 120, 0, 500, "%d ms")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        fov_ = settings.Float("fov");
        toggleMode_ = settings.Choice("mode") == "toggle";
        key_ = settings.Key("key");
        transitionSeconds_ = static_cast<float>(settings.Int("transition-ms")) / 1000.0F;
        active_ = false;
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); active_ = false; }
    void OnKeyboard(const KeyboardEvent& event) override {
        std::scoped_lock lock(mutex_);
        if (key_ == 0 || event.key != key_) return;
        if (!event.pressed) {
            if (!toggleMode_) active_ = false;
            return;
        }
        if (event.uiCaptured || event.repeated) return;
        active_ = toggleMode_ ? !active_ : true;
    }
    void Contribute(PresentationProfile& profile) const override {
        std::scoped_lock lock(mutex_);
        if (active_) profile.zoom = ZoomRequest{fov_, transitionSeconds_};
    }

private:
    float fov_{30.0F};
    float transitionSeconds_{0.12F};
    std::uint32_t key_{'C'};
    bool toggleMode_{};
    bool active_{};
};

class FreelookModule final : public VisualModule<IPresentationModule> {
public:
    FreelookModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            ChoiceSetting("mode", "Activation", "hold", {{"hold", "Hold"}, {"toggle", "Toggle"}}),
            KeySetting("key", "Activation key", 0x12, "Defaults to Left Alt."),
            FloatSetting("sensitivity", "Sensitivity", 1.0, 0.1, 3.0, "%.2fx"),
            IntSetting("return-ms", "Return transition", 150, 0, 500, "%d ms")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        toggleMode_ = settings.Choice("mode") == "toggle";
        key_ = settings.Key("key");
        sensitivity_ = settings.Float("sensitivity");
        returnSeconds_ = static_cast<float>(settings.Int("return-ms")) / 1000.0F;
        active_ = false;
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); active_ = false; }
    void OnKeyboard(const KeyboardEvent& event) override {
        std::scoped_lock lock(mutex_);
        if (key_ == 0 || event.key != key_) return;
        if (!event.pressed) {
            if (!toggleMode_) active_ = false;
            return;
        }
        if (event.uiCaptured || event.repeated) return;
        active_ = toggleMode_ ? !active_ : true;
    }
    void Contribute(PresentationProfile& profile) const override {
        std::scoped_lock lock(mutex_);
        profile.cameraRotation = CameraRotationRequest{active_, sensitivity_, returnSeconds_};
    }

private:
    float sensitivity_{1.0F};
    float returnSeconds_{0.15F};
    std::uint32_t key_{0x12};
    bool toggleMode_{};
    bool active_{};
};

class CinematicCameraModule final : public VisualModule<IPresentationModule> {
public:
    CinematicCameraModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("strength", "Smoothing", 0.75, 0.0, 1.0, "%.2f"),
            IntSetting("transition-ms", "Transition", 120, 0, 500, "%d ms")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_ = {settings.Float("strength"), static_cast<float>(settings.Int("transition-ms")) / 1000.0F};
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.cinematicCamera = request_; }

private:
    CinematicCameraRequest request_{};
};

class ThirdPersonDistanceModule final : public VisualModule<IPresentationModule> {
public:
    ThirdPersonDistanceModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("distance", "Camera distance", 4.0, 1.0, 20.0, "%.1f blocks"),
            BoolSetting("rear", "Rear camera", true),
            BoolSetting("front", "Front camera", true)}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_ = {settings.Float("distance"), settings.Bool("front"), settings.Bool("rear")};
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.thirdPersonDistance = request_; }

private:
    ThirdPersonDistanceRequest request_{};
};

class FullbrightModule final : public VisualModule<IPresentationModule> {
public:
    FullbrightModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {FloatSetting("multiplier", "Brightness", 5.0, 1.0, 10.0, "%.1fx")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override { std::scoped_lock lock(mutex_); request_.multiplier = settings.Float("multiplier"); }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.lighting = request_; }

private:
    LightingRequest request_{};
};

class TimeChangerModule final : public VisualModule<IPresentationModule> {
public:
    TimeChangerModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {IntSetting("ticks", "Frozen time", 6000, 0, 23999, "%d ticks")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override { std::scoped_lock lock(mutex_); request_.ticks = settings.Int("ticks"); }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.time = request_; }

private:
    TimeRequest request_{};
};

class WeatherSuppressionModule final : public VisualModule<IPresentationModule> {
public:
    WeatherSuppressionModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("rain", "Rain visibility", 0.0, 0.0, 100.0, "%.0f%%"),
            FloatSetting("snow", "Snow visibility", 0.0, 0.0, 100.0, "%.0f%%",
                "Follows rain for now: the game keeps rain and snow as one precipitation level."),
            FloatSetting("thunder", "Thunder intensity", 0.0, 0.0, 100.0, "%.0f%%")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_ = {settings.Float("rain") / 100.0F, settings.Float("snow") / 100.0F, settings.Float("thunder") / 100.0F};
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.weather = request_; }

private:
    WeatherRequest request_{};
};

class ScreenOverlaySuppressionModule final : public VisualModule<IPresentationModule> {
public:
    ScreenOverlaySuppressionModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("fire-opacity", "Fire opacity", 0.0, 0.0, 100.0, "%.0f%%", "Not supported yet; this overlay is unchanged."),
            FloatSetting("powder-snow-opacity", "Powder snow opacity", 0.0, 0.0, 100.0, "%.0f%%", "Not supported yet; this overlay is unchanged."),
            FloatSetting("portal-opacity", "Portal opacity", 0.0, 0.0, 100.0, "%.0f%%", "Not supported yet; this overlay is unchanged."),
            FloatSetting("pumpkin-opacity", "Pumpkin opacity", 0.0, 0.0, 100.0, "%.0f%%", "Not supported yet; this overlay is unchanged."),
            FloatSetting("vignette-opacity", "Vignette opacity", 0.0, 0.0, 100.0, "%.0f%%",
                "0% hides the vignette. Partial opacity is not supported yet; above 0 keeps vanilla."),
            FloatSetting("nausea-opacity", "Nausea opacity", 0.0, 0.0, 100.0, "%.0f%%", "Not supported yet; this overlay is unchanged.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_ = {settings.Float("fire-opacity") / 100.0F, settings.Float("powder-snow-opacity") / 100.0F,
            settings.Float("portal-opacity") / 100.0F, settings.Float("pumpkin-opacity") / 100.0F,
            settings.Float("vignette-opacity") / 100.0F, settings.Float("nausea-opacity") / 100.0F};
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.screenEffects = request_; }

private:
    ScreenEffectsRequest request_{};
};

class HurtCameraModule final : public VisualModule<IPresentationModule> {
public:
    HurtCameraModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {FloatSetting("strength", "Camera shake", 0.0, 0.0, 100.0, "%.0f%%")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_.strength = settings.Float("strength") / 100.0F;
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.hurtCamera = request_; }

private:
    HurtCameraRequest request_{};
};
}

std::unique_ptr<IModule> MakeCustomCrosshair(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<CustomCrosshairModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeBlockOutline(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<BlockOutlineModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeHurtColor(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<HurtColorModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeMotionBlur(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<MotionBlurModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeFovProfiles(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<FovProfilesModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeViewBobbing(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ViewBobbingModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeHandView(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<HandViewModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeZoom(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ZoomModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeFreelook(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<FreelookModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeCinematicCamera(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<CinematicCameraModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeThirdPersonDistance(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ThirdPersonDistanceModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeFullbright(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<FullbrightModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeTimeChanger(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<TimeChangerModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeWeatherSuppression(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<WeatherSuppressionModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeScreenOverlaySuppression(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ScreenOverlaySuppressionModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeHurtCamera(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<HurtCameraModule>(std::move(m), l, s); }

}
