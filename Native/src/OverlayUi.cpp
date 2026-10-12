#include "Oinkster/OverlayUi.hpp"
#include "Oinkster/ClickGui.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/Dx12Overlay.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/MouseInputBlocker.hpp"
#include "Oinkster/SkinHead.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include <Windows.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Oinkster::OverlayUi {
namespace {
using Clock = std::chrono::steady_clock;

enum class Section { Home, Modules, Hud, Settings };
enum class ControlCapture { None, Menu, Unload };
enum class HubIcon { Pencil, Modules, Settings };

// A pink-tinted dark ramp plus the Oinkster pink accent. Green/Amber only ever mark state,
// never decoration. Dx12Overlay.cpp mirrors these as floats for the stock
// ImGui widgets - keep the two in sync.
constexpr ImU32 Canvas = IM_COL32(24, 18, 22, 250);
constexpr ImU32 Rail = IM_COL32(30, 22, 27, 255);
constexpr ImU32 Surface = IM_COL32(38, 28, 34, 255);
constexpr ImU32 SurfaceRaised = IM_COL32(50, 36, 44, 255);
constexpr ImU32 Stroke = IM_COL32(68, 46, 57, 255);
constexpr ImU32 Text = IM_COL32(236, 236, 240, 255);
constexpr ImU32 Secondary = IM_COL32(204, 186, 196, 255);
constexpr ImU32 Muted = IM_COL32(160, 138, 150, 255);
constexpr ImU32 Pink = IM_COL32(240, 138, 176, 255);
constexpr ImU32 PinkDeep = IM_COL32(74, 40, 55, 255);
constexpr ImU32 Green = IM_COL32(104, 196, 128, 255);
constexpr ImU32 Amber = IM_COL32(224, 173, 98, 255);

Section section{Section::Home};
char search[96]{};
// Module whose settings are expanded in the list; empty when all are collapsed.
std::string expandedModule;
std::unique_ptr<ClickGui> moduleBrowser;
ModuleManager* moduleBrowserOwner{};
std::string capturingModule;
// "<module-id>.<key>" of a Key-type setting waiting for a key press.
std::string capturingSetting;
Clock::time_point sessionStarted{};
std::atomic_bool sessionActive{};
ControlCapture controlCapture{};
std::string controlMessage;
bool showCapabilities{};
SkinHeadTexture playerHeadTexture{};
std::uint64_t playerHeadRevision{};
bool playerHeadReady{};
std::mutex playerHeadMutex;
bool menuWasVisible{};
float headYaw{};
float headPitch{8.0F};
float headYawVelocity{};
float headPitchVelocity{};
ImFont* compactFont{};
ImFont* bodyFont{};
ImFont* titleFont{};

ImVec4 ToVec4(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }
ImFont* CompactFont() { return compactFont ? compactFont : ImGui::GetFont(); }
ImFont* BodyFont() { return bodyFont ? bodyFont : ImGui::GetFont(); }
ImFont* TitleFont() { return titleFont ? titleFont : ImGui::GetFont(); }
ImU32 FromArgb(std::uint32_t color) { return IM_COL32((color>>16)&0xFFU,(color>>8)&0xFFU,color&0xFFU,(color>>24)&0xFFU); }
std::uint32_t ToArgb(const ImVec4& color) { return (static_cast<std::uint32_t>(color.w*255.0F)<<24)|(static_cast<std::uint32_t>(color.x*255.0F)<<16)|(static_cast<std::uint32_t>(color.y*255.0F)<<8)|static_cast<std::uint32_t>(color.z*255.0F); }

std::string Narrow(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) result.push_back(character <= 0x7f ? static_cast<char>(character) : '?');
    return result;
}

std::string KeyName(std::uint32_t key) {
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) return std::string(1, static_cast<char>(key));
    char name[32]{};
    const auto scanCode = MapVirtualKeyA(key, MAPVK_VK_TO_VSC);
    if (scanCode != 0 && GetKeyNameTextA(static_cast<LONG>(scanCode << 16U), name, static_cast<int>(std::size(name))) > 0) {
        return name;
    }
    return std::format("VK {}", key);
}

ImU32 WithAlpha(ImU32 color, float alpha) {
    auto value = ToVec4(color);
    value.w *= std::clamp(alpha, 0.0F, 1.0F);
    return ImGui::ColorConvertFloat4ToU32(value);
}

const char* CategoryName(ModuleCategory value) {
    switch (value) {
    case ModuleCategory::Performance: return "Performance";
    case ModuleCategory::Input: return "Input";
    case ModuleCategory::Telemetry: return "Telemetry";
    case ModuleCategory::Status: return "Status";
    case ModuleCategory::PvpFeedback: return "PvP feedback";
    case ModuleCategory::Visual: return "Visual";
    case ModuleCategory::Accessibility: return "Accessibility";
    }
    return "Other";
}


void DrawSnout(ImDrawList* draw, ImVec2 center, float scale = 1.0F, float alpha = 1.0F) {
    draw->AddRectFilled(ImVec2(center.x - 18.0F * scale, center.y - 13.0F * scale),
        ImVec2(center.x + 18.0F * scale, center.y + 13.0F * scale), WithAlpha(Pink, alpha), 10.0F * scale);
    draw->AddCircleFilled(ImVec2(center.x - 7.0F * scale, center.y), 3.1F * scale, WithAlpha(PinkDeep, alpha));
    draw->AddCircleFilled(ImVec2(center.x + 7.0F * scale, center.y), 3.1F * scale, WithAlpha(PinkDeep, alpha));
}

void DrawHubIcon(ImDrawList* draw, ImVec2 center, float scale, HubIcon icon, ImU32 color) {
    if (icon == HubIcon::Modules) return;

    if (icon == HubIcon::Pencil) {
        const ImVec2 direction(0.70F, -0.70F);
        const ImVec2 normal(0.70F, 0.70F);
        const float halfLength = 24.0F * scale;
        const float halfWidth = 7.0F * scale;
        const ImVec2 a(center.x - direction.x * halfLength - normal.x * halfWidth,
            center.y - direction.y * halfLength - normal.y * halfWidth);
        const ImVec2 b(center.x + direction.x * halfLength - normal.x * halfWidth,
            center.y + direction.y * halfLength - normal.y * halfWidth);
        const ImVec2 c(center.x + direction.x * halfLength + normal.x * halfWidth,
            center.y + direction.y * halfLength + normal.y * halfWidth);
        const ImVec2 d(center.x - direction.x * halfLength + normal.x * halfWidth,
            center.y - direction.y * halfLength + normal.y * halfWidth);
        draw->AddQuad(a, b, c, d, color, 3.0F * scale);
        const ImVec2 tip(center.x + direction.x * (halfLength + 12.0F * scale),
            center.y + direction.y * (halfLength + 12.0F * scale));
        draw->AddTriangle(b, c, tip, color, 3.0F * scale);
        draw->AddLine(ImVec2(a.x + direction.x * 10.0F * scale, a.y + direction.y * 10.0F * scale),
            ImVec2(d.x + direction.x * 10.0F * scale, d.y + direction.y * 10.0F * scale), color, 3.0F * scale);
        return;
    }

    const float outerRadius = 25.0F * scale;
    const float innerRadius = 12.0F * scale;
    constexpr int Teeth = 10;
    std::array<ImVec2, Teeth * 2> points{};
    for (int index = 0; index < Teeth * 2; ++index) {
        const float angle = static_cast<float>(index) * 3.14159265F / static_cast<float>(Teeth);
        const float radius = index % 2 == 0 ? outerRadius : outerRadius * 0.78F;
        points[static_cast<std::size_t>(index)] = ImVec2(center.x + std::cos(angle) * radius,
            center.y + std::sin(angle) * radius);
    }
    draw->AddPolyline(points.data(), static_cast<int>(points.size()), color, ImDrawFlags_Closed, 3.0F * scale);
    draw->AddCircle(center, innerRadius, color, 0, 3.0F * scale);
}

struct Vec3 { float x; float y; float z; };
struct ProjectedVertex { ImVec2 screen; float depth; };
struct CubeFace { SkinFace texture; std::array<Vec3, 4> corners; Vec3 normal; };

constexpr std::array<CubeFace, SkinHeadTexture::FaceCount> CubeFaces{{
    {SkinFace::Right, {{{-1, 1, -1}, {-1, 1, 1}, {-1, -1, 1}, {-1, -1, -1}}}, {-1, 0, 0}},
    {SkinFace::Front, {{{-1, 1, 1}, {1, 1, 1}, {1, -1, 1}, {-1, -1, 1}}}, {0, 0, 1}},
    {SkinFace::Left, {{{1, 1, 1}, {1, 1, -1}, {1, -1, -1}, {1, -1, 1}}}, {1, 0, 0}},
    {SkinFace::Back, {{{1, 1, -1}, {-1, 1, -1}, {-1, -1, -1}, {1, -1, -1}}}, {0, 0, -1}},
    {SkinFace::Top, {{{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}}}, {0, 1, 0}},
    {SkinFace::Bottom, {{{-1, -1, 1}, {1, -1, 1}, {1, -1, -1}, {-1, -1, -1}}}, {0, -1, 0}}
}};

Vec3 Rotate(Vec3 value, float yawDegrees, float pitchDegrees) {
    constexpr float Radians = 3.1415926535F / 180.0F;
    const float yaw = yawDegrees * Radians;
    const float pitch = pitchDegrees * Radians;
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const Vec3 yawed{cy * value.x + sy * value.z, value.y, -sy * value.x + cy * value.z};
    return {yawed.x, cp * yawed.y - sp * yawed.z, sp * yawed.y + cp * yawed.z};
}

ProjectedVertex Project(Vec3 value, ImVec2 center, float halfSize) {
    constexpr float CameraDistance = 4.2F;
    const float perspective = CameraDistance / (CameraDistance - value.z);
    return {{center.x + value.x * halfSize * perspective,
        center.y - value.y * halfSize * perspective}, value.z};
}

ImVec2 Bilinear(const std::array<ProjectedVertex, 4>& corners, float u, float v) {
    const ImVec2 top(corners[0].screen.x + (corners[1].screen.x - corners[0].screen.x) * u,
        corners[0].screen.y + (corners[1].screen.y - corners[0].screen.y) * u);
    const ImVec2 bottom(corners[3].screen.x + (corners[2].screen.x - corners[3].screen.x) * u,
        corners[3].screen.y + (corners[2].screen.y - corners[3].screen.y) * u);
    return {top.x + (bottom.x - top.x) * v, top.y + (bottom.y - top.y) * v};
}

ImU32 ShadePixel(ImU32 color, float light) {
    const auto alpha = (color >> 24U) & 0xFFU;
    const auto red = static_cast<unsigned>(std::clamp(static_cast<float>(color & 0xFFU) * light, 0.0F, 255.0F));
    const auto green = static_cast<unsigned>(std::clamp(static_cast<float>((color >> 8U) & 0xFFU) * light, 0.0F, 255.0F));
    const auto blue = static_cast<unsigned>(std::clamp(static_cast<float>((color >> 16U) & 0xFFU) * light, 0.0F, 255.0F));
    return red | (green << 8U) | (blue << 16U) | (alpha << 24U);
}

void DrawCubeLayer(ImDrawList* draw, ImVec2 center, float size, float yaw, float pitch,
    const std::array<SkinHeadTexture::Face, SkinHeadTexture::FaceCount>& textures,
    std::uint32_t faceEdge, float expansion, bool transparent) {
    struct VisibleFace {
        const CubeFace* geometry;
        std::array<ProjectedVertex, 4> projected;
        float depth;
        float light;
    };
    std::vector<VisibleFace> visible;
    visible.reserve(3);
    for (const auto& face : CubeFaces) {
        const Vec3 normal = Rotate(face.normal, yaw, pitch);
        if (normal.z <= 0.015F) continue;
        VisibleFace projected{&face, {}, 0.0F,
            std::clamp(0.72F + normal.z * 0.30F + normal.y * 0.08F, 0.68F, 1.08F)};
        for (std::size_t corner = 0; corner < face.corners.size(); ++corner) {
            Vec3 vertex = face.corners[corner];
            vertex.x *= expansion;
            vertex.y *= expansion;
            vertex.z *= expansion;
            projected.projected[corner] = Project(Rotate(vertex, yaw, pitch), center, size * 0.5F);
            projected.depth += projected.projected[corner].depth;
        }
        projected.depth *= 0.25F;
        visible.push_back(projected);
    }
    std::ranges::sort(visible, {}, &VisibleFace::depth);
    // One solid quad per skin pixel, like Minecraft itself draws heads. Fill
    // anti-aliasing is switched off for these: every quad would otherwise get a
    // translucent fringe, and neighbouring fringes show up as a seam grid.
    // Adjacent quads share exact corner positions, so non-AA fills tile with
    // no gaps. The AA outline below keeps the cube's silhouette smooth.
    const std::size_t edge = std::clamp<std::size_t>(faceEdge, 1, SkinHeadTexture::MaxFaceEdge);
    const float edgeF = static_cast<float>(edge);
    for (const auto& face : visible) {
        const auto& pixels = textures[static_cast<std::size_t>(face.geometry->texture)];
        const ImDrawListFlags savedFlags = draw->Flags;
        draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (std::size_t y = 0; y < edge; ++y) {
            for (std::size_t x = 0; x < edge; ++x) {
                const ImU32 color = pixels[y * edge + x];
                if (((color >> 24U) & 0xFFU) == 0) continue;
                const float u0 = static_cast<float>(x) / edgeF;
                const float v0 = static_cast<float>(y) / edgeF;
                const float u1 = static_cast<float>(x + 1) / edgeF;
                const float v1 = static_cast<float>(y + 1) / edgeF;
                draw->AddQuadFilled(Bilinear(face.projected, u0, v0), Bilinear(face.projected, u1, v0),
                    Bilinear(face.projected, u1, v1), Bilinear(face.projected, u0, v1),
                    ShadePixel(color, face.light));
            }
        }
        draw->Flags = savedFlags;
        const ImVec2 outline[4]{face.projected[0].screen, face.projected[1].screen,
            face.projected[2].screen, face.projected[3].screen};
        draw->AddPolyline(outline, 4, IM_COL32(12, 15, 18, transparent ? 72 : 118), ImDrawFlags_Closed, 1.0F);
    }
}

bool SkinReady() {
    std::scoped_lock lock(playerHeadMutex);
    return playerHeadReady;
}

// Stand-in head used until PlayerSkinTelemetry delivers the real skin (and if
// it never does - e.g. the pattern scan missed on this Minecraft build). The
// home hub previously drew nothing at all in that window, leaving its three
// tiles floating under empty space.
//
// Built as a normal SkinHeadTexture so it goes through the same cube renderer
// as a real skin: it rotates and tracks the cursor identically, no separate
// 2D drawing path to keep in sync.
const SkinHeadTexture& FallbackHead() {
    static const SkinHeadTexture fallback = [] {
        constexpr std::array<std::array<int, 8>, 8> face{{
            {{1, 1, 1, 1, 1, 1, 1, 1}}, {{1, 1, 1, 1, 1, 1, 1, 1}},
            {{1, 2, 2, 1, 1, 2, 2, 1}}, {{1, 2, 2, 1, 1, 2, 2, 1}},
            {{1, 1, 1, 1, 1, 1, 1, 1}}, {{1, 1, 3, 3, 3, 3, 1, 1}},
            {{1, 1, 1, 1, 1, 1, 1, 1}}, {{1, 1, 1, 1, 1, 1, 1, 1}}
        }};
        // Deliberately the overlay palette's own pinks rather than skin tones:
        // this should read as "no skin loaded yet", not as a real player.
        constexpr std::array<std::uint32_t, 4> palette{
            IM_COL32(244, 158, 184, 255), IM_COL32(236, 140, 171, 255),
            IM_COL32(52, 35, 48, 255), IM_COL32(176, 78, 112, 255)
        };
        SkinHeadTexture result{};
        for (std::size_t faceIndex = 0; faceIndex < SkinHeadTexture::FaceCount; ++faceIndex) {
            auto& pixels = result.base[faceIndex];
            const bool isFront = faceIndex == static_cast<std::size_t>(SkinFace::Front);
            for (std::size_t y = 0; y < 8; ++y) {
                for (std::size_t x = 0; x < 8; ++x) {
                    pixels[y * 8 + x] = isFront
                        ? palette[static_cast<std::size_t>(face[y][x])]
                        : palette[1];
                }
            }
        }
        return result;
    }();
    return fallback;
}

void DrawSkinHead(ImDrawList* draw, ImVec2 center, float size, float deltaSeconds) {
    const auto& io = ImGui::GetIO();
    const bool pointerInside = io.MousePos.x >= 0.0F && io.MousePos.y >= 0.0F
        && io.MousePos.x <= io.DisplaySize.x && io.MousePos.y <= io.DisplaySize.y;
    const float normalizedX = pointerInside
        ? std::clamp((io.MousePos.x - center.x) / std::max(io.DisplaySize.x * 0.38F, 1.0F), -1.0F, 1.0F) : 0.0F;
    const float normalizedY = pointerInside
        ? std::clamp((io.MousePos.y - center.y) / std::max(io.DisplaySize.y * 0.38F, 1.0F), -1.0F, 1.0F) : 0.0F;
    headYaw = StepCriticallyDamped(headYaw, headYawVelocity, normalizedX * 32.0F, deltaSeconds, 0.09F);
    headPitch = StepCriticallyDamped(headPitch, headPitchVelocity, 6.0F + normalizedY * 20.0F, deltaSeconds, 0.09F);

    SkinHeadTexture snapshot{};
    bool ready{};
    {
        std::scoped_lock lock(playerHeadMutex);
        ready = playerHeadReady;
        if (ready) snapshot = playerHeadTexture;
    }
    draw->AddEllipseFilled(ImVec2(center.x, center.y + size * 0.63F), ImVec2(size * 0.48F, size * 0.095F),
        IM_COL32(0, 0, 0, 78));
    if (!ready) {
        DrawCubeLayer(draw, center, size, headYaw, headPitch, FallbackHead().base,
            FallbackHead().faceEdge, 1.0F, false);
        return;
    }
    DrawCubeLayer(draw, center, size, headYaw, headPitch, snapshot.base, snapshot.faceEdge, 1.0F, false);
    DrawCubeLayer(draw, center, size, headYaw, headPitch, snapshot.overlay, snapshot.faceEdge, 1.075F, true);
}

void DrawToast() {
    if (!sessionActive.load()) return;
    const float elapsed = std::chrono::duration<float>(Clock::now() - sessionStarted).count();
    constexpr float Duration = 4.2F;
    if (elapsed > Duration) {
        sessionActive.store(false);
        return;
    }
    const auto display = ImGui::GetIO().DisplaySize;
    const float alpha = elapsed < 0.2F ? elapsed / 0.2F
        : elapsed > Duration - 0.4F ? (Duration - elapsed) / 0.4F : 1.0F;
    const float reveal = 1.0F - std::pow(1.0F - std::clamp(elapsed / 0.3F, 0.0F, 1.0F), 3.0F);
    const char* title = "Oinkster loaded";
    const std::string hint = std::format("Press {} to open", KeyName(Dx12Overlay::MenuKey()));
    ImFont* caption = CompactFont();
    const ImVec2 titleSize = ImGui::CalcTextSize(title);
    const ImVec2 hintSize = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0F, hint.c_str());
    const ImVec2 size(14.0F + 24.0F + 10.0F + titleSize.x + 14.0F + hintSize.x + 16.0F, 40.0F);
    const ImVec2 position((display.x - size.x) * 0.5F, 20.0F - (1.0F - reveal) * 10.0F);
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::Begin("##oinkster-ready", nullptr, ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 maximum(position.x + size.x, position.y + size.y);
    draw->AddRectFilled(position, maximum, WithAlpha(Canvas, alpha), 8.0F);
    draw->AddRect(position, maximum, WithAlpha(Pink, alpha * 0.35F), 8.0F, 0, 1.0F);
    float x = position.x + 14.0F;
    DrawSnout(draw, ImVec2(x + 12.0F, position.y + size.y * 0.5F), 0.6F, alpha);
    x += 24.0F + 10.0F;
    draw->AddText(ImVec2(x, position.y + (size.y - titleSize.y) * 0.5F), WithAlpha(Text, alpha), title);
    x += titleSize.x + 14.0F;
    draw->AddText(caption, caption->FontSize, ImVec2(x, position.y + (size.y - hintSize.y) * 0.5F),
        WithAlpha(Muted, alpha), hint.c_str());
    ImGui::End();
    ImGui::PopStyleVar();
}


ImU32 ToneColor(HudTone tone, const HudLayout& layout) {
    switch (tone) {
    case HudTone::Accent: return FromArgb(layout.accentColor);
    case HudTone::Muted: return WithAlpha(FromArgb(layout.textColor), 0.6F);
    case HudTone::Warning: return Amber;
    case HudTone::Good: return Green;
    case HudTone::Normal: break;
    }
    return FromArgb(layout.textColor);
}

ImU32 LerpColor(ImU32 from, ImU32 to, float t) {
    const auto channel = [&](int shift) {
        const float a = static_cast<float>((from >> shift) & 0xFFU);
        const float b = static_cast<float>((to >> shift) & 0xFFU);
        return static_cast<ImU32>(std::lround(a + (b - a) * t)) << shift;
    };
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) | channel(IM_COL32_A_SHIFT);
}

// Per-key press intensity (0 idle .. 1 held), eased every frame so keys fade
// rather than snap. Keyed by "<module>/<row>/<column>"; render thread only.
std::unordered_map<std::string, float> keyPressFade;

struct HudWindowStability {
    float scale{};
    float minimumWidth{};
};

// Numeric HUD text changes width frequently (FPS every sample, coordinates
// whenever a digit boundary is crossed). AlwaysAutoResize otherwise makes the
// panel edge pulse every frame. Retain the widest size observed at the current
// scale for the rest of the injected session.
std::unordered_map<std::string, HudWindowStability> hudWindowStability;

// Draws a key grid as one block: every row is laid out across the same total
// width (the widest row's column count), so a key spanning w columns is
// w*unit + (w-1)*gap wide and all rows sit flush with each other.
void DrawKeyRows(const std::string& moduleId, const std::vector<std::vector<HudKey>>& rows, const HudLayout& layout) {
    const float unit = 34.0F * layout.scale;
    const float gap = 3.0F * layout.scale;
    const float rounding = 4.0F * layout.scale;
    float columns = 0.0F;
    for (const auto& row : rows) {
        float span = 0.0F;
        for (const auto& key : row) span += key.widthUnits;
        columns = std::max(columns, span);
    }
    if (columns <= 0.0F) return;
    const float totalWidth = columns * unit + (columns - 1.0F) * gap;
    const auto rowHeight = [&](const std::vector<HudKey>& row) {
        bool bar = !row.empty(), sub = false;
        for (const auto& key : row) {
            bar = bar && key.bar;
            sub = sub || !key.subLabel.empty();
        }
        return bar ? unit * 0.62F : sub ? unit * 1.3F : unit;
    };

    const float delta = std::clamp(ImGui::GetIO().DeltaTime, 0.0F, 0.1F);
    const ImU32 accent = FromArgb(layout.accentColor);
    const ImU32 text = FromArgb(layout.textColor);
    const ImU32 idleFill = WithAlpha(Canvas, 0.62F);
    const ImU32 pressedFill = WithAlpha(accent, 0.92F);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    float y = origin.y;
    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const auto& row = rows[rowIndex];
        const float height = rowHeight(row);
        float span = 0.0F;
        for (const auto& key : row) span += key.widthUnits;
        // A row narrower than the grid is centered.
        float x = origin.x + (columns - span) * (unit + gap) * 0.5F;
        for (std::size_t column = 0; column < row.size(); ++column) {
            const auto& key = row[column];
            const float width = key.widthUnits * unit + (key.widthUnits - 1.0F) * gap;
            if (!key.spacer) {
                float& fade = keyPressFade[std::format("{}/{}/{}", moduleId, rowIndex, column)];
                const float rate = key.pressed ? 1.0F / 0.07F : 1.0F / 0.14F;
                fade = std::clamp(fade + (key.pressed ? rate : -rate) * delta, 0.0F, 1.0F);
                const ImVec2 minimum(x, y);
                const ImVec2 maximum(x + width, y + height);
                draw->AddRectFilled(minimum, maximum, LerpColor(idleFill, pressedFill, fade), rounding);
                draw->AddRect(minimum, maximum, LerpColor(WithAlpha(Stroke, 0.9F), accent, fade), rounding, 0, 1.0F);
                const ImU32 labelColor = LerpColor(text, Canvas, fade);
                if (key.bar) {
                    const float barHalf = width * 0.22F;
                    const float centerY = y + height * 0.5F;
                    draw->AddLine(ImVec2(x + width * 0.5F - barHalf, centerY), ImVec2(x + width * 0.5F + barHalf, centerY),
                        labelColor, std::max(1.5F, 2.0F * layout.scale));
                } else if (key.subLabel.empty()) {
                    const ImVec2 size = ImGui::CalcTextSize(key.label.c_str());
                    draw->AddText(ImVec2(x + (width - size.x) * 0.5F, y + (height - size.y) * 0.5F), labelColor, key.label.c_str());
                } else {
                    // Label plus a smaller caption (e.g. "7 CPS") stacked and centered.
                    ImFont* font = ImGui::GetFont();
                    const float fontSize = ImGui::GetFontSize();
                    const float subSize = fontSize * 0.72F;
                    const ImVec2 labelSize = ImGui::CalcTextSize(key.label.c_str());
                    const ImVec2 captionSize = font->CalcTextSizeA(subSize, FLT_MAX, 0.0F, key.subLabel.c_str());
                    const float top = y + (height - labelSize.y - captionSize.y) * 0.5F;
                    draw->AddText(ImVec2(x + (width - labelSize.x) * 0.5F, top), labelColor, key.label.c_str());
                    draw->AddText(font, subSize, ImVec2(x + (width - captionSize.x) * 0.5F, top + labelSize.y),
                        WithAlpha(labelColor, 0.75F), key.subLabel.c_str());
                }
            }
            x += width + gap;
        }
        y += height + (rowIndex + 1 < rows.size() ? gap : 0.0F);
    }
    ImGui::Dummy(ImVec2(totalWidth, y - origin.y));
}

bool DrawHud(ModuleManager& modules, bool editMode) {
    bool dirty{};
    const auto display = ImGui::GetIO().DisplaySize;
    auto hudModules = modules.EnabledHudModules();
    std::ranges::sort(hudModules, {}, [](const IHudModule* module) { return module->Metadata().id; });
    for (auto* module : hudModules) {
        HudContent content;
        try { content = module->Content(); } catch (...) { continue; }
        const bool empty = content.lines.empty() && !content.graph && content.keyRows.empty();
        // A module with nothing to say right now (e.g. "hide when OK") takes
        // no space - except in edit mode, where it must stay draggable.
        if (empty && !editMode) continue;
        auto layout = module->Layout();
        if (!editMode) ImGui::SetNextWindowPos(ImVec2(layout.normalizedX * display.x, layout.normalizedY * display.y), ImGuiCond_Always);
        else ImGui::SetNextWindowPos(ImVec2(layout.normalizedX * display.x, layout.normalizedY * display.y), ImGuiCond_Appearing);
        // Bare content (key grids) draws its own tiles; a panel behind them
        // only adds a box around the keys. Edit mode keeps the border so it
        // can still be found and dragged.
        const bool bare = content.bare && !empty;
        ImGui::SetNextWindowBgAlpha(layout.background && !bare ? layout.backgroundOpacity : 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, bare ? ImVec2(2.0F, 2.0F) : ImVec2(10.0F * layout.scale, 6.0F * layout.scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, editMode ? 1.0F : 0.0F);
        auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing;
        if (!editMode) flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(FromArgb(layout.backgroundColor)));
        ImGui::PushStyleColor(ImGuiCol_Border, ToVec4(FromArgb(layout.accentColor)));
        const std::string id = "##hud-" + module->Metadata().id;
        auto& stability = hudWindowStability[module->Metadata().id];
        if (std::abs(stability.scale - layout.scale) > 0.001F) {
            stability.scale = layout.scale;
            stability.minimumWidth = 0.0F;
        }
        if (stability.minimumWidth > 0.0F) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(stability.minimumWidth, 0.0F), ImVec2(FLT_MAX, FLT_MAX));
        }
        ImGui::Begin(id.c_str(), nullptr, flags);
        ImGui::SetWindowFontScale(layout.scale);
        if (empty) ImGui::TextColored(ToVec4(Muted), "%s", Narrow(module->Metadata().name).c_str());
        for (const auto& line : content.lines) {
            ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(ToneColor(line.tone, layout)));
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopStyleColor();
        }
        if (!content.keyRows.empty()) DrawKeyRows(module->Metadata().id, content.keyRows, layout);
        if (content.graph && !content.graph->values.empty()) {
            const auto& graph = *content.graph;
            ImGui::PushStyleColor(ImGuiCol_PlotLines, ToVec4(FromArgb(layout.accentColor)));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
            ImGui::PlotLines("##graph", graph.values.data(), static_cast<int>(graph.values.size()), 0,
                graph.caption.empty() ? nullptr : graph.caption.c_str(), graph.minimum, graph.maximum,
                ImVec2(220.0F * layout.scale, 62.0F * layout.scale));
            ImGui::PopStyleColor(2);
        }
        const auto position = ImGui::GetWindowPos();
        const auto size = ImGui::GetWindowSize();
        stability.minimumWidth = std::max(stability.minimumWidth, size.x);
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        if (editMode && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            HudLayout next = layout;
            next.normalizedX = std::clamp(position.x / std::max(display.x, 1.0F), 0.0F,
                std::max(0.0F, 1.0F - size.x / std::max(display.x, 1.0F)));
            next.normalizedY = std::clamp(position.y / std::max(display.y, 1.0F), 0.0F,
                std::max(0.0F, 1.0F - size.y / std::max(display.y, 1.0F)));
            if (next.normalizedX != layout.normalizedX || next.normalizedY != layout.normalizedY) {
                module->SetLayout(next);
                dirty = true;
            }
        }
    }
    return dirty;
}

void DrawOverlayPrimitives(ModuleManager& modules, ImVec2 display) {
    auto* draw = ImGui::GetBackgroundDrawList();
    for (const auto* module : modules.EnabledModulesOf<IOverlayModule>()) {
        OverlayPrimitives primitives;
        try { primitives = module->Primitives(display.x, display.y); } catch (...) { continue; }
        for (const auto& rect : primitives.rects) {
            if (rect.filled) draw->AddRectFilled(ImVec2(rect.x1, rect.y1), ImVec2(rect.x2, rect.y2), FromArgb(rect.argb));
            else draw->AddRect(ImVec2(rect.x1, rect.y1), ImVec2(rect.x2, rect.y2), FromArgb(rect.argb), 0.0F, 0, rect.thickness);
        }
        for (const auto& line : primitives.lines) {
            draw->AddLine(ImVec2(line.x1, line.y1), ImVec2(line.x2, line.y2), FromArgb(line.argb), line.thickness);
        }
        for (const auto& circle : primitives.circles) {
            if (circle.filled) draw->AddCircleFilled(ImVec2(circle.x, circle.y), circle.radius, FromArgb(circle.argb));
            else draw->AddCircle(ImVec2(circle.x, circle.y), circle.radius, FromArgb(circle.argb), 0, circle.thickness);
        }
    }
}

bool HubTile(const char* id, ImVec2 position, ImVec2 size, HubIcon icon, const char* label, float scale,
    const char* tooltip = nullptr) {
    ImGui::SetCursorScreenPos(position);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    // The icon-only tiles carry no text at all, so without this there is
    // nothing to tell the player what they open.
    if (hovered && tooltip) ImGui::SetTooltip("%s", tooltip);
    const bool active = ImGui::IsItemActive();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 maximum(position.x + size.x, position.y + size.y);
    const float rounding = 10.0F * scale;
    draw->AddRectFilled(ImVec2(position.x + 4.0F * scale, position.y + 6.0F * scale),
        ImVec2(maximum.x + 4.0F * scale, maximum.y + 6.0F * scale), IM_COL32(0, 0, 0, 72), rounding);
    const ImU32 fill = active ? LerpColor(PinkDeep, Pink, 0.25F) : hovered ? PinkDeep : Surface;
    draw->AddRectFilled(position, maximum, fill, rounding);
    draw->AddRect(position, maximum, hovered ? Pink : WithAlpha(Pink, 0.35F), rounding, 0, hovered ? 1.8F : 1.0F);

    const ImU32 foreground = hovered ? Pink : Text;
    if (icon == HubIcon::Modules) {
        ImGui::PushFont(TitleFont());
        const ImVec2 labelSize = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(position.x + (size.x - labelSize.x) * 0.5F,
            position.y + (size.y - labelSize.y) * 0.5F), foreground, label);
        ImGui::PopFont();
    } else {
        DrawHubIcon(draw, ImVec2(position.x + size.x * 0.5F, position.y + size.y * 0.5F), scale, icon, foreground);
    }
    return pressed;
}

void DrawHomeHub(ModuleManager& modules, float deltaSeconds) {
    const auto display = ImGui::GetIO().DisplaySize;
    const float resolutionScale = std::clamp(std::min(display.x / 1280.0F, display.y / 720.0F), 0.85F, 1.15F);
    const float scale = resolutionScale * ParseGuiScale(modules.ModuleSetting("click-gui.scale", "0.65"));
    const ImVec2 sideSize(172.0F * scale, 126.0F * scale);
    const ImVec2 centerSize(362.0F * scale, 126.0F * scale);
    const float gap = 16.0F * scale;
    const float dockWidth = sideSize.x * 2.0F + centerSize.x + gap * 2.0F;
    const float dockY = display.y * 0.58F;
    const float startX = (display.x - dockWidth) * 0.5F;
    DrawSkinHead(ImGui::GetWindowDrawList(), ImVec2(display.x * 0.5F, dockY - 104.0F * scale),
        132.0F * scale, deltaSeconds);
    if (HubTile("##customize-hub", ImVec2(startX, dockY), sideSize, HubIcon::Pencil,
        "", scale, "HUD layout - arrange your HUD")) section = Section::Hud;
    if (HubTile("##modules-hub", ImVec2(startX + sideSize.x + gap, dockY), centerSize,
        HubIcon::Modules, "MODS", scale, "Modules - browse fair-play tools")) section = Section::Modules;
    if (HubTile("##settings-hub", ImVec2(startX + sideSize.x + gap + centerSize.x + gap, dockY),
        sideSize, HubIcon::Settings, "", scale, "Settings - controls and status")) section = Section::Settings;
}

// Middle dot, spelled as bytes so it does not depend on the source charset.
constexpr const char* Dot = " \xC2\xB7 ";

std::string Ellipsize(ImFont* font, const std::string& text, float maxWidth) {
    const auto width = [&](const std::string& value) {
        return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, value.c_str()).x;
    };
    if (width(text) <= maxWidth) return text;
    const float ellipsis = width("...");
    std::string result = text;
    while (!result.empty() && width(result) + ellipsis > maxWidth) result.pop_back();
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result + "...";
}

void CaptionText(ImU32 color, const char* text) {
    ImGui::PushFont(CompactFont());
    ImGui::TextColored(ToVec4(color), "%s", text);
    ImGui::PopFont();
}

void GroupLabel(const char* label) {
    ImGui::Dummy(ImVec2(0.0F, 6.0F));
    CaptionText(Pink, label);
    ImGui::Dummy(ImVec2(0.0F, 2.0F));
}

void PageHeader(const char* title, const char* subtitle) {
    ImGui::PushFont(TitleFont());
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    const ImVec2 underline(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 1.0F);
    ImGui::GetWindowDrawList()->AddRectFilled(underline, ImVec2(underline.x + 28.0F, underline.y + 3.0F), Pink, 1.5F);
    ImGui::Dummy(ImVec2(0.0F, 2.0F));
    if (subtitle) CaptionText(Muted, subtitle);
}

// Animated on/off switch, the one control used for every boolean.
bool Switch(const char* id, bool on, bool interactive = true) {
    constexpr float Width = 36.0F;
    constexpr float Height = 20.0F;
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(Width, Height)) && interactive;
    const bool hovered = interactive && ImGui::IsItemHovered();
    auto* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetItemID();
    float t = storage->GetFloat(key, on ? 1.0F : 0.0F);
    const float step = std::clamp(ImGui::GetIO().DeltaTime, 0.0F, 0.1F) / 0.12F;
    t = on ? std::min(1.0F, t + step) : std::max(0.0F, t - step);
    storage->SetFloat(key, t);
    const float alpha = interactive ? 1.0F : 0.45F;
    const ImU32 offTrack = hovered ? IM_COL32(62, 62, 72, 255) : Stroke;
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(position, ImVec2(position.x + Width, position.y + Height),
        WithAlpha(LerpColor(offTrack, Pink, t), alpha), Height * 0.5F);
    const ImVec2 knob(position.x + Height * 0.5F + t * (Width - Height), position.y + Height * 0.5F);
    draw->AddCircleFilled(knob, Height * 0.5F - 3.0F,
        WithAlpha(LerpColor(Secondary, IM_COL32(255, 255, 255, 255), t), alpha));
    return pressed;
}

bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0.0F, 0.0F)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ToVec4(Pink));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToVec4(IM_COL32(246, 160, 192, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToVec4(IM_COL32(222, 120, 158, 255)));
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(IM_COL32(32, 18, 25, 255)));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

// Quiet text-only button for secondary actions such as resets.
bool LinkButton(const char* id, const char* label) {
    ImFont* font = CompactFont();
    const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, label);
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    ImGui::GetWindowDrawList()->AddText(font, font->FontSize, position,
        ImGui::IsItemHovered() ? Text : Muted, label);
    return pressed;
}

bool FavoriteButton(bool favorite, bool visible, bool& hovered) {
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##favorite", ImVec2(24.0F, 24.0F));
    hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetTooltip(favorite ? "Remove from favorites" : "Add to favorites");
    if (!favorite && !visible && !hovered) return pressed;
    const ImVec2 center(position.x + 12.0F, position.y + 12.0F);
    std::array<ImVec2, 10> points{};
    for (std::size_t index = 0; index < points.size(); ++index) {
        const float angle = -1.5707963F + static_cast<float>(index) * 0.6283185F;
        const float radius = index % 2 == 0 ? 7.0F : 3.1F;
        points[index] = ImVec2(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
    }
    auto* draw = ImGui::GetWindowDrawList();
    if (favorite) draw->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), Pink);
    else draw->AddPolyline(points.data(), static_cast<int>(points.size()), hovered ? Text : Muted, ImDrawFlags_Closed, 1.3F);
    return pressed;
}

void DrawChevron(ImDrawList* draw, ImVec2 center, bool open, ImU32 color) {
    constexpr float Size = 4.0F;
    const std::array<ImVec2, 3> points = open
        ? std::array<ImVec2, 3>{ImVec2(center.x - Size, center.y - Size * 0.5F), ImVec2(center.x, center.y + Size * 0.5F),
            ImVec2(center.x + Size, center.y - Size * 0.5F)}
        : std::array<ImVec2, 3>{ImVec2(center.x - Size * 0.5F, center.y - Size), ImVec2(center.x + Size * 0.5F, center.y),
            ImVec2(center.x - Size * 0.5F, center.y + Size)};
    draw->AddPolyline(points.data(), static_cast<int>(points.size()), color, ImDrawFlags_None, 1.6F);
}

// Next key pressed since the last poll, or 0. Only called while a rebind is pending.
int PollPressedKey() {
    for (int key = 8; key < 256; ++key) {
        if ((GetAsyncKeyState(key) & 1) != 0) return key;
    }
    return 0;
}

bool KeyButton(const char* id, std::uint32_t key, bool capturing, float width) {
    const std::string label = (capturing ? std::string("Press a key...") : key == 0 ? std::string("None") : KeyName(key)) + id;
    if (capturing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ToVec4(PinkDeep));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToVec4(PinkDeep));
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(Pink));
    }
    const bool pressed = ImGui::Button(label.c_str(), ImVec2(width, 0.0F));
    if (capturing) ImGui::PopStyleColor(3);
    return pressed;
}

// Two-column "label | control" layout used by every settings form.
struct Form {
    float labelWidth;
    float controlWidth;
};

Form MakeForm(float width) {
    const float label = std::clamp(width * 0.42F, 120.0F, 220.0F);
    return {label, std::min(width - label, 300.0F)};
}

// Draws the label and leaves the cursor at the start of the control column.
void FormLabel(const Form& form, const std::string& label, const char* help = nullptr) {
    const float startX = ImGui::GetCursorScreenPos().x;
    const std::string shown = Ellipsize(ImGui::GetFont(), label, form.labelWidth - 12.0F);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ToVec4(Secondary), "%s", shown.c_str());
    if (ImGui::IsItemHovered() && ((help && *help) || shown != label)) {
        ImGui::SetTooltip("%s%s%s", label.c_str(), help && *help ? "\n" : "", help ? help : "");
    }
    ImGui::SameLine(0.0F, 0.0F);
    ImGui::SetCursorScreenPos(ImVec2(startX + form.labelWidth, ImGui::GetCursorScreenPos().y));
}

// Moves the cursor to the control column for a follow-up line (hints, warnings).
void FormIndent(const Form& form, float rowStartX) {
    ImGui::SetCursorScreenPos(ImVec2(rowStartX + form.labelWidth, ImGui::GetCursorScreenPos().y));
}

bool FormSwitch(const Form& form, const std::string& label, const char* id, bool on, const char* help = nullptr) {
    FormLabel(form, label, help);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + (ImGui::GetFrameHeight() - 20.0F) * 0.5F));
    return Switch(id, on);
}

void CaptureHint(const Form& form, float rowStartX) {
    FormIndent(form, rowStartX);
    CaptionText(Muted, "Esc cancels, Backspace clears");
}

bool DrawSettingControl(ModuleManager& modules, const std::string& moduleId, const SettingDescriptor& descriptor,
    const SettingsReader& reader, const Form& form) {
    const std::string fullKey = moduleId + "." + descriptor.key;
    const std::string id = "##setting-" + descriptor.key;
    const char* help = descriptor.help.empty() ? nullptr : descriptor.help.c_str();
    const float rowStartX = ImGui::GetCursorScreenPos().x;
    bool changed = false;
    std::string value;
    ImGui::PushID(descriptor.key.c_str());
    switch (descriptor.type) {
    case SettingType::Bool: {
        const bool current = reader.Bool(descriptor.key);
        if (FormSwitch(form, descriptor.label, id.c_str(), current, help)) { value = current ? "false" : "true"; changed = true; }
        break;
    }
    case SettingType::Int: {
        int current = reader.Int(descriptor.key);
        FormLabel(form, descriptor.label, help);
        ImGui::SetNextItemWidth(form.controlWidth);
        if (ImGui::SliderInt(id.c_str(), &current, static_cast<int>(descriptor.minimum), static_cast<int>(descriptor.maximum),
                descriptor.format.empty() ? "%d" : descriptor.format.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
            value = std::to_string(current);
            changed = true;
        }
        break;
    }
    case SettingType::Float: {
        float current = reader.Float(descriptor.key);
        FormLabel(form, descriptor.label, help);
        ImGui::SetNextItemWidth(form.controlWidth);
        if (ImGui::SliderFloat(id.c_str(), &current, static_cast<float>(descriptor.minimum), static_cast<float>(descriptor.maximum),
                descriptor.format.empty() ? "%.2f" : descriptor.format.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
            value = FormatNumberSetting(current, SettingType::Float);
            changed = true;
        }
        break;
    }
    case SettingType::Color: {
        auto color = ToVec4(FromArgb(reader.Color(descriptor.key)));
        FormLabel(form, descriptor.label, help);
        if (ImGui::ColorEdit4(id.c_str(), &color.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                ImGuiColorEditFlags_AlphaBar)) {
            value = FormatColorSetting(ToArgb(color));
            changed = true;
        }
        break;
    }
    case SettingType::Choice: {
        const auto current = reader.Choice(descriptor.key);
        const auto selected = std::ranges::find(descriptor.choices, current, &SettingChoice::value);
        FormLabel(form, descriptor.label, help);
        ImGui::SetNextItemWidth(form.controlWidth);
        if (ImGui::BeginCombo(id.c_str(), selected == descriptor.choices.end() ? "" : selected->label.c_str())) {
            for (const auto& choice : descriptor.choices) {
                if (ImGui::Selectable(choice.label.c_str(), choice.value == current)) { value = choice.value; changed = true; }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case SettingType::Key: {
        const bool capturing = capturingSetting == fullKey;
        FormLabel(form, descriptor.label, help);
        if (KeyButton(id.c_str(), reader.Key(descriptor.key), capturing, std::min(form.controlWidth, 160.0F))) {
            capturingSetting = fullKey;
            capturingModule.clear();
        }
        if (capturing) {
            if (const int key = PollPressedKey(); key != 0) {
                const auto code = static_cast<std::uint32_t>(key);
                if (key == VK_ESCAPE) capturingSetting.clear();
                else if (key == VK_BACK) { value = "0"; changed = true; capturingSetting.clear(); }
                else if (code != Dx12Overlay::MenuKey() && code != Dx12Overlay::UnloadKey() && !IsReservedKey(code)) {
                    value = std::to_string(code);
                    changed = true;
                    capturingSetting.clear();
                }
            }
            CaptureHint(form, rowStartX);
        }
        break;
    }
    case SettingType::Text: {
        std::array<char, MaxTextSettingLength + 1> buffer{};
        const auto current = reader.Text(descriptor.key);
        std::copy_n(current.begin(), std::min(current.size(), MaxTextSettingLength), buffer.begin());
        FormLabel(form, descriptor.label, help);
        ImGui::SetNextItemWidth(form.controlWidth);
        if (ImGui::InputText(id.c_str(), buffer.data(), buffer.size())) { value = buffer.data(); changed = true; }
        break;
    }
    }
    ImGui::PopID();
    if (changed) modules.SetModuleSetting(fullKey, value);
    return changed;
}

void ResetHud(ModuleManager& modules) {
    for (auto* module : modules.EnabledHudModules()) module->SetLayout(module->DefaultLayout());
}

// Everything configurable about one module, shown inline under its row.
void DrawModulePanel(ModuleManager& modules, const ModuleMetadata& metadata, float width, bool& dirty) {
    const Form form = MakeForm(width);
    auto* module = modules.FindModule(metadata.id);

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::PushFont(CompactFont());
    ImGui::TextColored(ToVec4(Muted), "%s", Narrow(metadata.description).c_str());
    if (!metadata.available) {
        ImGui::Dummy(ImVec2(0.0F, 2.0F));
        ImGui::TextColored(ToVec4(Amber), "%s", Narrow(metadata.unavailableReason).c_str());
        for (const auto capability : metadata.requiredCapabilities) {
            const auto status = modules.CapabilityStatusOf(capability);
            if (status.IsAvailable()) continue;
            ImGui::TextColored(ToVec4(Muted), "Needs %s%s", Narrow(CapabilityName(capability)).c_str(),
                status.state == CapabilityState::Pending ? " (checking)" : "");
        }
        if (modules.IsRequested(metadata.id)) {
            ImGui::TextColored(ToVec4(Muted), "You turned this on - it starts automatically once available.");
        }
    }
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0.0F, 4.0F));

    if (metadata.available) {
        const float rowStartX = ImGui::GetCursorScreenPos().x;
        const bool capturing = capturingModule == metadata.id;
        FormLabel(form, "Keybind", "Toggles this module while playing.");
        if (KeyButton("##module-bind", modules.Keybind(metadata.id), capturing, std::min(form.controlWidth, 160.0F))) {
            capturingModule = metadata.id;
            capturingSetting.clear();
        }
        if (capturing) {
            if (const int key = PollPressedKey(); key != 0) {
                if (key == VK_ESCAPE) capturingModule.clear();
                else if (key == VK_BACK) {
                    (void)modules.SetKeybind(metadata.id, 0, Dx12Overlay::MenuKey(), Dx12Overlay::UnloadKey());
                    capturingModule.clear();
                    dirty = true;
                } else if (modules.SetKeybind(metadata.id, static_cast<std::uint32_t>(key), Dx12Overlay::MenuKey(),
                        Dx12Overlay::UnloadKey())) {
                    capturingModule.clear();
                    dirty = true;
                }
            }
            CaptureHint(form, rowStartX);
        }
        const auto conflicts = modules.KeybindConflicts(metadata.id);
        if (!conflicts.empty()) {
            std::string names;
            for (const auto& other : conflicts) names += (names.empty() ? "" : ", ") + other;
            FormIndent(form, rowStartX);
            const std::string warning = "Also toggles " + names;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + form.controlWidth);
            CaptionText(Amber, warning.c_str());
            ImGui::PopTextWrapPos();
        }
        if (module && !module->Actions().empty()) {
            FormLabel(form, "Actions");
            bool first = true;
            for (const auto& action : module->Actions()) {
                if (!first) ImGui::SameLine();
                first = false;
                if (ImGui::Button((action.label + "##action-" + action.id).c_str())) {
                    (void)modules.InvokeAction(metadata.id, action.id, std::chrono::steady_clock::now());
                }
            }
        }
    }

    auto* hud = metadata.available ? modules.FindHudModule(metadata.id) : nullptr;
    if (hud) {
        auto layout = hud->Layout();
        bool changed = false;
        GroupLabel("Appearance");
        FormLabel(form, "Size");
        ImGui::SetNextItemWidth(form.controlWidth);
        changed |= ImGui::SliderFloat("##scale", &layout.scale, 0.65F, 2.0F, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        if (FormSwitch(form, "Background", "##background", layout.background)) {
            layout.background = !layout.background;
            changed = true;
        }
        if (layout.background) {
            float opacityPercent = layout.backgroundOpacity * 100.0F;
            FormLabel(form, "Background opacity");
            ImGui::SetNextItemWidth(form.controlWidth);
            if (ImGui::SliderFloat("##opacity", &opacityPercent, 0.0F, 100.0F, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
                layout.backgroundOpacity = opacityPercent / 100.0F;
                changed = true;
            }
        }
        const auto colorRow = [&](const char* label, const char* id, std::uint32_t& argb) {
            auto color = ToVec4(FromArgb(argb));
            FormLabel(form, label);
            if (ImGui::ColorEdit4(id, &color.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                argb = ToArgb(color);
                changed = true;
            }
        };
        colorRow("Text color", "##text-color", layout.textColor);
        colorRow("Accent color", "##accent-color", layout.accentColor);
        if (layout.background) colorRow("Background color", "##background-color", layout.backgroundColor);
        if (changed) {
            hud->SetLayout(layout);
            dirty = true;
        }
    }

    const bool hasSettings = module && !module->Settings().empty();
    if (hasSettings) {
        GroupLabel("Settings");
        if (!metadata.available) CaptionText(Muted, "Saved now, applied once the module is available.");
        const auto reader = modules.SettingsFor(metadata.id);
        for (const auto& descriptor : module->Settings()) {
            if (DrawSettingControl(modules, metadata.id, descriptor, reader, form)) dirty = true;
        }
    }

    if (hud || hasSettings) {
        ImGui::Dummy(ImVec2(0.0F, 4.0F));
        if (LinkButton("##reset-module", "Reset to defaults")) {
            if (hasSettings) modules.ResetModuleSettings(metadata.id);
            if (hud) {
                // Defaults for look and feel only; leave the widget where the player put it.
                const auto current = hud->Layout();
                auto next = hud->DefaultLayout();
                next.normalizedX = current.normalizedX;
                next.normalizedY = current.normalizedY;
                hud->SetLayout(next);
            }
            dirty = true;
        }
    }
}

void DrawModuleRow(ModuleManager& modules, ClickGui& browser, const ModuleMetadata& metadata, float width, bool& dirty) {
    constexpr float RowHeight = 56.0F;
    constexpr float Pad = 14.0F;
    ImGui::PushID(metadata.id.c_str());
    const bool expanded = expandedModule == metadata.id;
    const bool enabled = modules.IsEnabled(metadata.id);
    const bool favorite = browser.IsFavorite(metadata.id);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // The row background depends on the expanded panel's height, which is only
    // known after drawing it: content goes to channel 1, background to 0.
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);

    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("##row", ImVec2(width, RowHeight))) {
        expandedModule = expanded ? std::string{} : metadata.id;
    }
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
    const float centerY = origin.y + RowHeight * 0.5F;
    float controlsLeft = origin.x + width - Pad - 10.0F;
    DrawChevron(draw, ImVec2(controlsLeft + 5.0F, centerY), expanded, hovered || expanded ? Text : Muted);

    if (metadata.available) {
        controlsLeft -= 14.0F + 36.0F;
        ImGui::SetCursorScreenPos(ImVec2(controlsLeft, centerY - 10.0F));
        if (Switch("##enabled", enabled) && browser.ToggleModule(metadata.id)) dirty = true;
    } else {
        ImFont* font = CompactFont();
        const char* label = "Unavailable";
        const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, label);
        controlsLeft -= 14.0F + size.x;
        draw->AddText(font, font->FontSize, ImVec2(controlsLeft, centerY - size.y * 0.5F), Muted, label);
    }
    controlsLeft -= 8.0F + 24.0F;
    ImGui::SetCursorScreenPos(ImVec2(controlsLeft, centerY - 12.0F));
    bool starHovered{};
    if (FavoriteButton(favorite, hovered || expanded, starHovered) && browser.ToggleFavorite(metadata.id)) dirty = true;

    const float textWidth = controlsLeft - 12.0F - (origin.x + Pad);
    const std::string name = Ellipsize(BodyFont(), Narrow(metadata.name), textWidth);
    draw->AddText(BodyFont(), BodyFont()->FontSize, ImVec2(origin.x + Pad, origin.y + 9.0F),
        metadata.available ? Text : Secondary, name.c_str());
    const std::string detail = expanded ? std::string(CategoryName(metadata.category))
        : Ellipsize(CompactFont(), Narrow(metadata.description), textWidth);
    draw->AddText(CompactFont(), CompactFont()->FontSize, ImVec2(origin.x + Pad, origin.y + 32.0F), Muted, detail.c_str());

    float bottom = origin.y + RowHeight;
    if (expanded) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + Pad, bottom));
        ImGui::BeginGroup();
        DrawModulePanel(modules, metadata, width - Pad * 2.0F, dirty);
        ImGui::EndGroup();
        bottom = ImGui::GetItemRectMax().y + Pad;
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom));
    ImGui::Dummy(ImVec2(width, 0.0F));

    draw->ChannelsSetCurrent(0);
    const ImVec2 maximum(origin.x + width, bottom);
    if (expanded) {
        draw->AddRectFilled(origin, maximum, Surface, 8.0F);
        draw->AddRectFilled(ImVec2(origin.x, origin.y + 10.0F), ImVec2(origin.x + 3.0F, maximum.y - 10.0F), Pink, 1.5F);
        draw->AddLine(ImVec2(origin.x + Pad, origin.y + RowHeight), ImVec2(maximum.x - Pad, origin.y + RowHeight), Stroke);
    } else if (hovered) {
        draw->AddRectFilled(origin, maximum, LerpColor(Surface, PinkDeep, 0.5F), 8.0F);
    }
    draw->ChannelsMerge();
    ImGui::PopID();
}

const char* ViewName(const ClickGui& browser) {
    switch (browser.View()) {
    case ModuleView::All: return "All modules";
    case ModuleView::Favorites: return "Favorites";
    case ModuleView::Enabled: return "Enabled";
    case ModuleView::Available: return "Available";
    case ModuleView::Roadmap: return "Roadmap";
    case ModuleView::Category:
        return browser.Category() ? CategoryName(*browser.Category()) : "Category";
    }
    return "Modules";
}

void DrawModulesPage(ModuleManager& modules, ClickGui& browser, bool& dirty) {
    browser.SetSearch(std::wstring(search, search + std::strlen(search)));
    const auto visible = browser.FilteredCatalog();
    const auto enabledCount = std::ranges::count_if(visible, [&](const ModuleMetadata& item) { return modules.IsEnabled(item.id); });
    const bool searching = search[0] != '\0';

    const ImVec2 headerStart = ImGui::GetCursorPos();
    const float headerWidth = ImGui::GetContentRegionAvail().x;
    const std::string subtitle = std::format("{} modules{}{} on", visible.size(), Dot, enabledCount);
    PageHeader(ViewName(browser), subtitle.c_str());
    const float headerEnd = ImGui::GetCursorPosY();

    const float searchWidth = std::clamp(headerWidth * 0.4F, 160.0F, 260.0F);
    ImGui::SetCursorPos(ImVec2(headerStart.x + headerWidth - searchWidth, headerStart.y + 6.0F));
    ImGui::SetNextItemWidth(searchWidth);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InputTextWithHint("##module-search", "Search", search, sizeof(search));
    if (searching) {
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        const float side = maximum.y - minimum.y;
        ImGui::SetCursorScreenPos(ImVec2(maximum.x - side, minimum.y));
        if (ImGui::InvisibleButton("##clear-search", ImVec2(side, side))) search[0] = '\0';
        const ImU32 color = ImGui::IsItemHovered() ? Text : Muted;
        const ImVec2 center(maximum.x - side * 0.5F, minimum.y + side * 0.5F);
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddLine(ImVec2(center.x - 4.0F, center.y - 4.0F), ImVec2(center.x + 4.0F, center.y + 4.0F), color, 1.5F);
        draw->AddLine(ImVec2(center.x - 4.0F, center.y + 4.0F), ImVec2(center.x + 4.0F, center.y - 4.0F), color, 1.5F);
    }
    ImGui::SetCursorPos(ImVec2(headerStart.x, headerEnd + 10.0F));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::BeginChild("module-list", ImVec2(0.0F, 0.0F), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    const float width = ImGui::GetContentRegionAvail().x;
    for (const auto& metadata : visible) DrawModuleRow(modules, browser, metadata, width, dirty);
    if (visible.empty()) {
        ImGui::Dummy(ImVec2(0.0F, 32.0F));
        ImGui::TextColored(ToVec4(Secondary), "%s", searching ? "No modules match your search." : "Nothing here yet.");
        CaptionText(Muted, searching ? "Try a different word, or clear the search."
            : browser.View() == ModuleView::Favorites ? "Star a module to pin it here."
            : "Pick another view on the left.");
    }
    ImGui::EndChild();
}

bool NavItem(const char* id, const char* label, std::optional<std::size_t> count, bool active) {
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 32.0F);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 maximum(position.x + size.x, position.y + size.y);
    if (active) {
        draw->AddRectFilled(position, maximum, PinkDeep, 6.0F);
        draw->AddRectFilled(ImVec2(position.x, position.y + 8.0F), ImVec2(position.x + 3.0F, maximum.y - 8.0F), Pink, 1.5F);
    } else if (hovered) {
        draw->AddRectFilled(position, maximum, Surface, 6.0F);
    }
    draw->AddText(ImVec2(position.x + 12.0F, position.y + (size.y - ImGui::GetFontSize()) * 0.5F),
        active || hovered ? Text : Secondary, label);
    if (count) {
        ImFont* font = CompactFont();
        const std::string text = std::to_string(*count);
        const ImVec2 textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, text.c_str());
        draw->AddText(font, font->FontSize, ImVec2(maximum.x - textSize.x - 10.0F, position.y + (size.y - textSize.y) * 0.5F),
            active ? Pink : Muted, text.c_str());
    }
    return pressed;
}

void DrawSidebar(ModuleManager& modules, ClickGui& browser, float deltaSeconds) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    DrawSkinHead(draw, ImVec2(origin.x + 22.0F, origin.y + 18.0F), 26.0F, deltaSeconds);
    draw->AddText(BodyFont(), BodyFont()->FontSize, ImVec2(origin.x + 46.0F, origin.y + 1.0F), Text, "Oinkster");
    const float nameWidth = BodyFont()->CalcTextSizeA(BodyFont()->FontSize, FLT_MAX, 0.0F, "Oinkster").x;
    DrawSnout(draw, ImVec2(origin.x + 46.0F + nameWidth + 16.0F, origin.y + 10.0F), 0.42F);
    draw->AddText(CompactFont(), CompactFont()->FontSize, ImVec2(origin.x + 46.0F, origin.y + 21.0F), Muted, "Esc to go back");
    ImGui::Dummy(ImVec2(0.0F, 46.0F));
    if (NavItem("##nav-home", "Home", std::nullopt, false)) section = Section::Home;
    ImGui::Dummy(ImVec2(0.0F, 6.0F));

    const auto catalog = modules.Catalog();
    const auto countWhere = [&](auto predicate) {
        return static_cast<std::size_t>(std::ranges::count_if(catalog, predicate));
    };
    const bool onModules = section == Section::Modules;
    const auto viewItem = [&](const char* id, const char* label, ModuleView view, std::size_t count) {
        if (NavItem(id, label, count, onModules && browser.View() == view)) {
            section = Section::Modules;
            browser.SetView(view);
        }
    };
    viewItem("##view-all", "All modules", ModuleView::All, catalog.size());
    viewItem("##view-enabled", "Enabled", ModuleView::Enabled,
        countWhere([&](const ModuleMetadata& item) { return modules.IsEnabled(item.id); }));
    viewItem("##view-favorites", "Favorites", ModuleView::Favorites,
        countWhere([&](const ModuleMetadata& item) { return browser.IsFavorite(item.id); }));

    ImGui::Dummy(ImVec2(0.0F, 8.0F));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0F);
    CaptionText(Pink, "Categories");
    ImGui::Dummy(ImVec2(0.0F, 2.0F));
    constexpr std::array categories{ModuleCategory::Performance, ModuleCategory::Input, ModuleCategory::Status,
        ModuleCategory::Telemetry, ModuleCategory::PvpFeedback, ModuleCategory::Visual, ModuleCategory::Accessibility};
    for (const auto category : categories) {
        const std::string id = std::format("##category-{}", static_cast<int>(category));
        const bool active = onModules && browser.View() == ModuleView::Category && browser.Category() == category;
        if (NavItem(id.c_str(), CategoryName(category),
                countWhere([&](const ModuleMetadata& item) { return item.category == category; }), active)) {
            section = Section::Modules;
            browser.SetCategory(category);
        }
    }

    // HUD and Settings sit at the bottom when there is room, apart from the module filters.
    const float bottomBlock = 32.0F * 2.0F + ImGui::GetStyle().ItemSpacing.y + 12.0F;
    const float pinnedY = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - bottomBlock;
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + 8.0F, pinnedY));
    const ImVec2 divider = ImGui::GetCursorScreenPos();
    draw->AddLine(ImVec2(divider.x, divider.y + 4.0F), ImVec2(divider.x + ImGui::GetContentRegionAvail().x, divider.y + 4.0F), Stroke);
    ImGui::Dummy(ImVec2(0.0F, 12.0F - ImGui::GetStyle().ItemSpacing.y));
    if (NavItem("##nav-hud", "HUD layout", std::nullopt, section == Section::Hud)) section = Section::Hud;
    if (NavItem("##nav-settings", "Settings", std::nullopt, section == Section::Settings)) section = Section::Settings;
}

void DrawHudPage(ModuleManager& modules, bool& editMode, bool& dirty) {
    PageHeader("HUD layout", "Pick the widgets you want while playing, then drag them into place.");
    ImGui::Dummy(ImVec2(0.0F, 10.0F));
    if (PrimaryButton("Edit layout", ImVec2(130.0F, 0.0F))) editMode = true;
    ImGui::SameLine();
    if (ImGui::Button("Reset layout")) {
        ResetHud(modules);
        dirty = true;
    }
    GroupLabel("Widgets");

    const float width = std::min(ImGui::GetContentRegionAvail().x, 640.0F);
    bool any = false;
    for (const auto& metadata : modules.Catalog()) {
        if (!modules.FindHudModule(metadata.id)) continue;
        any = true;
        constexpr float RowHeight = 40.0F;
        ImGui::PushID(metadata.id.c_str());
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        auto* draw = ImGui::GetWindowDrawList();
        if (ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + width, origin.y + RowHeight))) {
            draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + RowHeight), WithAlpha(Surface, 0.8F), 6.0F);
        }
        const float textY = origin.y + (RowHeight - ImGui::GetFontSize()) * 0.5F;
        draw->AddText(ImVec2(origin.x + 12.0F, textY), metadata.available ? Text : Secondary, Narrow(metadata.name).c_str());
        if (metadata.available) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + width - 12.0F - 36.0F, origin.y + (RowHeight - 20.0F) * 0.5F));
            const bool enabled = modules.IsEnabled(metadata.id);
            if (Switch("##enabled", enabled) && modules.SetEnabled(metadata.id, !enabled)) dirty = true;
        } else {
            ImFont* font = CompactFont();
            const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, "Unavailable");
            draw->AddText(font, font->FontSize, ImVec2(origin.x + width - 12.0F - size.x, origin.y + (RowHeight - size.y) * 0.5F),
                Muted, "Unavailable");
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + RowHeight));
        ImGui::Dummy(ImVec2(width, 0.0F));
        ImGui::PopID();
    }
    if (!any) CaptionText(Muted, "No HUD widgets are installed.");
}

void DrawSettingsPage(ModuleManager& modules, bool& dirty) {
    PageHeader("Settings", nullptr);
    const Form form = MakeForm(std::min(ImGui::GetContentRegionAvail().x, 600.0F));

    GroupLabel("Interface");
    float guiScalePercent = ParseGuiScale(modules.ModuleSetting("click-gui.scale", "0.65")) * 100.0F;
    FormLabel(form, "Home screen size", "Size of the head and tiles on the menu's home screen.");
    ImGui::SetNextItemWidth(form.controlWidth);
    if (ImGui::SliderFloat("##gui-scale", &guiScalePercent, 50.0F, 100.0F, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        modules.SetModuleSetting("click-gui.scale", std::format("{:.2f}", guiScalePercent / 100.0F));
        dirty = true;
    }

    GroupLabel("Controls");
    const float rowStartX = ImGui::GetCursorScreenPos().x;
    FormLabel(form, "Open menu");
    if (KeyButton("##menu-key", Dx12Overlay::MenuKey(), controlCapture == ControlCapture::Menu, 160.0F)) {
        controlCapture = ControlCapture::Menu;
        controlMessage.clear();
    }
    FormLabel(form, "Unload client");
    if (KeyButton("##unload-key", Dx12Overlay::UnloadKey(), controlCapture == ControlCapture::Unload, 160.0F)) {
        controlCapture = ControlCapture::Unload;
        controlMessage.clear();
    }
    if (controlCapture != ControlCapture::None) {
        if (const int pressed = PollPressedKey(); pressed != 0) {
            const auto key = static_cast<std::uint32_t>(pressed);
            const auto menu = Dx12Overlay::MenuKey();
            const auto unload = Dx12Overlay::UnloadKey();
            if (pressed == VK_ESCAPE) {
                controlCapture = ControlCapture::None;
            } else if ((controlCapture == ControlCapture::Menu && key == unload)
                || (controlCapture == ControlCapture::Unload && key == menu)) {
                controlMessage = "Menu and unload keys must be different.";
                controlCapture = ControlCapture::None;
            } else {
                Dx12Overlay::ConfigureControlKeys(controlCapture == ControlCapture::Menu ? key : menu,
                    controlCapture == ControlCapture::Unload ? key : unload);
                controlMessage.clear();
                dirty = true;
                controlCapture = ControlCapture::None;
            }
        }
        if (controlCapture != ControlCapture::None) {
            FormIndent(form, rowStartX);
            CaptionText(Muted, "Esc cancels");
        }
    }
    if (!controlMessage.empty()) {
        FormIndent(form, rowStartX);
        CaptionText(Amber, controlMessage.c_str());
    }

    // Live state rather than hardcoded labels, so problems are visible
    // in-game instead of only in oinkster-native.log.
    GroupLabel("Status");
    const auto statusRow = [&](const char* label, bool ok, const char* detail, const char* tooltip = nullptr) {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float lineHeight = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(start.x + 4.0F, start.y + lineHeight * 0.5F), 3.5F, ok ? Green : Muted);
        ImGui::SetCursorScreenPos(ImVec2(start.x + 16.0F, start.y));
        ImGui::TextColored(ToVec4(Secondary), "%s", label);
        if (tooltip && *tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine(0.0F, 0.0F);
        ImGui::SetCursorScreenPos(ImVec2(start.x + form.labelWidth, start.y + 2.0F));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + form.controlWidth);
        CaptionText(Muted, detail);
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos(ImVec2(start.x, ImGui::GetCursorScreenPos().y + 2.0F));
    };

    statusRow("Overlay", true, "Direct3D 12 present hook attached");
    switch (MouseInputBlocker::State()) {
    case MouseInputBlocker::InstallState::Ready:
        statusRow("Input routing", true, "Window scoping and GameInput mouse guard");
        break;
    case MouseInputBlocker::InstallState::Initializing:
        statusRow("Input routing", false, "Window scoping only - discovering GameInput");
        break;
    default:
        statusRow("Input routing", false, "Window scoping only - GameInput guard unavailable");
        break;
    }
    const auto telemetry = modules.CapabilityStatusOf(Capability::LocalPlayerPosition);
    statusRow("World telemetry", telemetry.IsAvailable(), telemetry.IsAvailable()
        ? "Camera position signature matched"
        : telemetry.state == CapabilityState::Pending ? "Validating signature" : "Unavailable for this build");
    statusRow("Player skin", SkinReady(), SkinReady()
        ? "Equipped skin loaded from the game"
        : "Not loaded yet - showing a placeholder head");

    ImGui::Dummy(ImVec2(0.0F, 4.0F));
    if (LinkButton("##toggle-capabilities", showCapabilities ? "Hide capabilities" : "Show all capabilities")) {
        showCapabilities = !showCapabilities;
    }
    if (showCapabilities) {
        ImGui::Dummy(ImVec2(0.0F, 2.0F));
        const auto capabilities = modules.Capabilities();
        for (std::size_t index = 0; index < capabilities.size(); ++index) {
            const auto& status = capabilities[index];
            const std::string name = Narrow(CapabilityName(static_cast<Capability>(index)));
            const char* state = status.IsAvailable() ? "Available"
                : status.state == CapabilityState::Pending ? "Checking" : "Unavailable";
            const std::string reason = Narrow(status.reason);
            statusRow(name.c_str(), status.IsAvailable(), state, reason.c_str());
        }
    }
}

void DrawEditOverlay(ModuleManager& modules, bool& editMode, bool& dirty) {
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0F, 0.0F), display, IM_COL32(0, 0, 0, 90));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5F, 24.0F), ImGuiCond_Always, ImVec2(0.5F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0F, 10.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0F, 6.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(Canvas));
    ImGui::PushStyleColor(ImGuiCol_Border, ToVec4(WithAlpha(Pink, 0.35F)));
    ImGui::Begin("##hud-edit-tools", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Editing HUD");
    ImGui::SameLine(0.0F, 12.0F);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0F);
    CaptionText(Muted, "Drag widgets to move them");
    ImGui::SameLine(0.0F, 20.0F);
    if (ImGui::Button("Reset")) {
        ResetHud(modules);
        dirty = true;
    }
    ImGui::SameLine();
    if (PrimaryButton("Done", ImVec2(80.0F, 0.0F))) editMode = false;
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}
}

void ConfigureFonts(ImFont* compact, ImFont* body, ImFont* title) noexcept {
    compactFont = compact;
    bodyFont = body;
    titleFont = title;
}

void BeginSession() noexcept {
    sessionStarted = Clock::now();
    sessionActive.store(true);
    search[0] = '\0';
    expandedModule.clear();
    hudWindowStability.clear();
    if (moduleBrowser) {
        moduleBrowser->SetSearch({});
        moduleBrowser->SetView(ModuleView::All);
    }
}

bool HasTransientUi() noexcept { return sessionActive.load(); }

bool UpdatePlayerSkin(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgbaPixels, std::uint64_t revision) noexcept {
    if ((width != 64 || height != 64) && (width != 128 || height != 128)) return false;
    const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    if (rgbaPixels.size() != expected || revision == 0) return false;
    SkinHeadTexture extracted{};
    if (!ExtractSkinHead(width, height, rgbaPixels, extracted)) return false;
    std::scoped_lock lock(playerHeadMutex);
    if (revision == playerHeadRevision) return true;
    playerHeadTexture = extracted;
    playerHeadRevision = revision;
    playerHeadReady = true;
    Diagnostics::LogLine("info", "player_skin_ui_texture", std::format(
        "head texture stored for render: revision={:#x} atlas={}x{}", revision, width, height));
    return true;
}

DrawResult Draw(ModuleManager& modules, bool menuVisible, bool gameplayHudVisible, bool& hudEditMode, const FrameInfo& frame) {
    DrawResult result{};
    const auto display = ImGui::GetIO().DisplaySize;
    if (moduleBrowserOwner != &modules) {
        moduleBrowser = std::make_unique<ClickGui>(modules);
        moduleBrowserOwner = &modules;
    }
    const float deltaSeconds = frame.deltaSeconds;
    modules.Publish(RenderEvent{display.x, display.y, 96.0F, frame.deltaSeconds, menuVisible, hudEditMode,
        frame.gameFocused, frame.frameIndex, frame.presentedAt, frame.keysDown});
    DrawToast();
    if (!menuVisible && gameplayHudVisible) DrawOverlayPrimitives(modules, display);
    if ((!menuVisible && gameplayHudVisible) || hudEditMode) result.settingsDirty |= DrawHud(modules, hudEditMode);
    if (!menuVisible) {
        menuWasVisible = false;
        return result;
    }
    if (!menuWasVisible) {
        section = Section::Home;
        headYaw = 0.0F;
        headPitch = 8.0F;
        headYawVelocity = 0.0F;
        headPitchVelocity = 0.0F;
        menuWasVisible = true;
    }
    if (hudEditMode) {
        DrawEditOverlay(modules, hudEditMode, result.settingsDirty);
        return result;
    }

    // Escape belongs to a pending rebind (it cancels it), not to the menu.
    const bool capturingKey = !capturingModule.empty() || !capturingSetting.empty() || controlCapture != ControlCapture::None;
    if (!capturingKey && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (search[0] != '\0') search[0] = '\0';
        else if (section != Section::Home) section = Section::Home;
        else result.closeRequested = true;
    }

    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0F, 0.0F), display, IM_COL32(0, 0, 0, 110));
    if (section == Section::Home) {
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
        ImGui::SetNextWindowSize(display, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::Begin("##oinkster-home", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        DrawHomeHub(modules, deltaSeconds);
        ImGui::End();
        ImGui::PopStyleVar();
        return result;
    }
    const ImVec2 size(std::min(std::clamp(display.x * 0.6F, 820.0F, 1060.0F), std::max(320.0F, display.x - 24.0F)),
        std::min(std::clamp(display.y * 0.72F, 560.0F, 760.0F), std::max(320.0F, display.y - 24.0F)));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5F, display.y * 0.5F), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0F, 6.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0F, 6.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10.0F);
    constexpr int StyleVars = 8;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(Canvas));
    ImGui::PushStyleColor(ImGuiCol_Border, ToVec4(WithAlpha(Pink, 0.35F)));
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(Text));
    constexpr int StyleColors = 3;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::Begin("##oinkster-menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    constexpr float SidebarWidth = 212.0F;
    const ImVec2 origin = ImGui::GetWindowPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(origin.x + 1.0F, origin.y + 1.0F), ImVec2(origin.x + SidebarWidth, origin.y + size.y - 1.0F),
        Rail, 9.0F, ImDrawFlags_RoundCornersLeft);
    draw->AddLine(ImVec2(origin.x + SidebarWidth, origin.y + 1.0F), ImVec2(origin.x + SidebarWidth, origin.y + size.y - 1.0F), Stroke);

    ImGui::SetCursorPos(ImVec2(0.0F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F, 16.0F));
    ImGui::BeginChild("##sidebar", ImVec2(SidebarWidth, size.y), ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0F, 2.0F));
    DrawSidebar(modules, *moduleBrowser, deltaSeconds);
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(SidebarWidth + 1.0F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0F, 20.0F));
    ImGui::BeginChild("##content", ImVec2(size.x - SidebarWidth - 1.0F, size.y), ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    switch (section) {
    case Section::Modules: DrawModulesPage(modules, *moduleBrowser, result.settingsDirty); break;
    case Section::Hud: DrawHudPage(modules, hudEditMode, result.settingsDirty); break;
    case Section::Settings: DrawSettingsPage(modules, result.settingsDirty); break;
    }
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleColor(StyleColors);
    ImGui::PopStyleVar(StyleVars);
    return result;
}
}
