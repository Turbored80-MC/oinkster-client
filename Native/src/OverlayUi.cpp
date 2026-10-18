#include "Oinkster/OverlayUi.hpp"
#include "Oinkster/ClickGui.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/Dx12Overlay.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/MouseInputBlocker.hpp"
#include "Oinkster/ReplayInjector.hpp"
#include "Oinkster/ReplayWorld.hpp"
#include "Oinkster/ReplayTimeline.hpp"
#include "Oinkster/SettingsStore.hpp"
#include "Oinkster/WorldTelemetry.hpp"
#include "Oinkster/SkinHead.hpp"
#include "Oinkster/VideoRecorder.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include <Windows.h>
#include <shellapi.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Oinkster::OverlayUi {
namespace {
using Clock = std::chrono::steady_clock;

enum class Section { Home, Modules, Profiles, Hud, Settings };
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
bool profilesLoaded{};
std::vector<std::string> profileNames;
std::string activeProfile;
std::string pendingDeleteProfile;
std::string profileMessage;
bool profileMessageIsError{};
std::array<char, 33> newProfileName{};
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

// Start / Pause / Mark / Finish / Discard for the active recorder. Shared by
// the pause-menu panel and the Recorder card on the Replays page.
void DrawRecorderControls(IReplayControlModule& replay, SteadyTime now, float buttonWidth) {
    const auto snapshot = replay.ReplaySnapshot(now);
    const bool canStart = snapshot.canStart;
    const auto seconds = snapshot.elapsed.count() / 1000;
    const auto stateLabel = snapshot.state == ReplayControlState::Recording ? "RECORDING"
        : snapshot.state == ReplayControlState::Paused ? "PAUSED"
        : snapshot.state == ReplayControlState::Error ? "ERROR" : "READY";
    ImGui::TextColored(snapshot.state == ReplayControlState::Recording ? ImVec4(0.95F, 0.35F, 0.45F, 1.0F)
        : ImVec4(0.72F, 0.74F, 0.77F, 1.0F), "%s  %02lld:%02lld  Marks %zu", stateLabel,
        static_cast<long long>(seconds / 60), static_cast<long long>(seconds % 60), snapshot.markers);
    const bool idle = snapshot.state == ReplayControlState::Idle || snapshot.state == ReplayControlState::Error;
    const bool active = snapshot.state == ReplayControlState::Recording || snapshot.state == ReplayControlState::Paused;
    const char* detail = snapshot.detail.c_str();
    if (detail[0] != '\0') {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + buttonWidth * 2.0F + 12.0F);
        ImGui::TextDisabled("%s", detail);
        ImGui::PopTextWrapPos();
    }

    const char* primary = snapshot.state == ReplayControlState::Recording ? "Pause"
        : snapshot.state == ReplayControlState::Paused ? "Resume" : "Start";
    ImGui::BeginDisabled(idle && !canStart);
    if (ImGui::Button(primary, ImVec2(buttonWidth, 31.0F))) {
        if (idle) (void)replay.StartReplay(now); else (void)replay.ToggleReplayPause(now);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!active);
    if (ImGui::Button("Mark", ImVec2(buttonWidth, 31.0F))) (void)replay.MarkReplay(now);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!active);
    if (ImGui::Button("Finish", ImVec2(buttonWidth, 31.0F))) (void)replay.FinishReplay(now);
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(buttonWidth, 31.0F))) ImGui::OpenPopup("Discard recording?");
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Discard recording?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Delete this unfinished recording?");
        if (ImGui::Button("Keep", ImVec2(100.0F, 0.0F))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(100.0F, 0.0F))) {
            (void)replay.DiscardReplay();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

ImU32 WithAlpha(ImU32 color, float alpha);
bool PrimaryButton(const char* label, ImVec2 size);

std::string ClockText(std::uint64_t micros) {
    const auto seconds = micros / 1'000'000;
    return std::format("{:02}:{:02}", seconds / 60, seconds % 60);
}

void ExitReplayButton(const char* id, float width, float height);

// Playback controls shown beside Minecraft's pause menu while watching.
void DrawPlaybackControls() {
    const auto status = ReplayInjector::CurrentStatus();
    ImGui::TextColored(ImVec4(0.95F, 0.55F, 0.70F, 1.0F), "%s  %s / %s", status.paused ? "PAUSED" : "WATCHING",
        ClockText(status.positionMicros).c_str(), ClockText(status.lengthMicros).c_str());
    ImGui::TextDisabled("%s", status.phaseText.c_str());
    if (ImGui::Button(status.paused ? "Play" : "Pause", ImVec2(124.0F, 31.0F))) ReplayInjector::SetPaused(!status.paused);
    ImGui::SameLine();
    if (ImGui::Button(std::format("Speed {}x", status.speed).c_str(), ImVec2(124.0F, 31.0F))) {
        constexpr std::array<float, 4> Speeds{0.5F, 1.0F, 2.0F, 4.0F};
        const auto current = std::ranges::find(Speeds, status.speed);
        ReplayInjector::SetSpeed(current == Speeds.end() || current + 1 == Speeds.end() ? Speeds[0] : *(current + 1));
    }
    ExitReplayButton("panel-exit", 124.0F, 31.0F);
}

// --- Replay editor dock ----------------------------------------------------
// The bottom timeline while a replay plays, laid out like Playback's editor:
// a toolbar (timeline zoom, transport, time), an overview strip, a ruler and
// track lanes under one orange playhead. It is interactive while Minecraft's
// pause screen is open (the cursor is free there); during playback a thin
// read-only bar takes its place.
namespace Dock {
// The Oinkster ramp (see the palette at the top of this file).
constexpr ImU32 Panel = IM_COL32(24, 18, 22, 246);
constexpr ImU32 Header = Rail;
constexpr ImU32 Lane = Surface;
constexpr ImU32 LaneAlt = IM_COL32(43, 32, 39, 255);
constexpr ImU32 Line = Stroke;
constexpr ImU32 Tick = IM_COL32(112, 86, 100, 255);
constexpr ImU32 Label = Muted;
constexpr ImU32 Time = Pink;
constexpr ImU32 Mark = Secondary;
constexpr ImU32 Playhead = Pink;
constexpr ImU32 PinkHover = IM_COL32(248, 164, 196, 255);
constexpr ImU32 Button = SurfaceRaised;
constexpr ImU32 ButtonHover = IM_COL32(66, 46, 57, 255);
constexpr float ToolbarHeight = 36.0F;
constexpr float OverviewHeight = 18.0F;
constexpr float RulerHeight = 26.0F;
constexpr float TrackHeight = 26.0F;
constexpr float LayersWidth = 150.0F;
constexpr float StepMicros = 5'000'000.0F;
constexpr std::array<float, 5> Speeds{0.25F, 0.5F, 1.0F, 2.0F, 4.0F};

// Visible fraction of the clip is 1/zoom; scroll is the visible start (0-1).
float zoom = 1.0F;
float scroll = 0.0F;
// A drag previews here and seeks on release, so dragging backwards does not
// rebuild the world on every frame.
std::optional<std::uint64_t> dragTarget;

enum class Icon { Start, Back, Play, Pause, Forward, End };

std::string Timecode(std::uint64_t micros) {
    const auto hundredths = micros / 10'000;
    return std::format("{:02}:{:02}.{:02}", hundredths / 6000, (hundredths / 100) % 60, hundredths % 100);
}

void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color) {
    const float h = size * 0.5F;
    const auto triangle = [&](float x, bool right) {
        const float d = right ? h : -h;
        draw->AddTriangleFilled(ImVec2(x - d * 0.8F, center.y - h), ImVec2(x - d * 0.8F, center.y + h),
            ImVec2(x + d * 0.8F, center.y), color);
    };
    switch (icon) {
    case Icon::Play: triangle(center.x + 1.0F, true); break;
    case Icon::Pause:
        draw->AddRectFilled(ImVec2(center.x - h * 0.8F, center.y - h), ImVec2(center.x - h * 0.2F, center.y + h), color, 1.0F);
        draw->AddRectFilled(ImVec2(center.x + h * 0.2F, center.y - h), ImVec2(center.x + h * 0.8F, center.y + h), color, 1.0F);
        break;
    case Icon::Back: triangle(center.x - h * 0.45F, false); triangle(center.x + h * 0.55F, false); break;
    case Icon::Forward: triangle(center.x - h * 0.55F, true); triangle(center.x + h * 0.45F, true); break;
    case Icon::Start:
        draw->AddRectFilled(ImVec2(center.x - h, center.y - h), ImVec2(center.x - h * 0.65F, center.y + h), color);
        triangle(center.x + h * 0.25F, false);
        break;
    case Icon::End:
        triangle(center.x - h * 0.25F, true);
        draw->AddRectFilled(ImVec2(center.x + h * 0.65F, center.y - h), ImVec2(center.x + h, center.y + h), color);
        break;
    }
}

bool IconButton(const char* id, Icon icon, float size, bool accent = false) {
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    if (accent || hovered) {
        draw->AddRectFilled(position, ImVec2(position.x + size, position.y + size),
            accent ? (hovered ? PinkHover : Pink) : ButtonHover, 6.0F);
    }
    DrawIcon(draw, icon, ImVec2(position.x + size * 0.5F, position.y + size * 0.5F), size * 0.36F,
        accent ? Canvas : hovered ? Text : Secondary);
    return clicked;
}

bool TextButton(const char* label, float width, float height) {
    ImGui::PushStyleColor(ImGuiCol_Button, ToVec4(Button));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToVec4(ButtonHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToVec4(Line));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0F);
    const bool clicked = ImGui::Button(label, ImVec2(width, height));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return clicked;
}

// A tick spacing that keeps labels about 90px apart.
std::uint64_t TickStep(double visibleMicros, float width) {
    constexpr std::array<std::uint64_t, 10> steps{100'000, 250'000, 500'000, 1'000'000, 2'000'000, 5'000'000,
        10'000'000, 30'000'000, 60'000'000, 300'000'000};
    const double wanted = visibleMicros * 90.0 / std::max(1.0F, width);
    for (const auto step : steps) {
        if (static_cast<double>(step) >= wanted) return step;
    }
    return steps.back();
}

void ClampView() {
    zoom = std::clamp(zoom, 1.0F, 64.0F);
    scroll = std::clamp(scroll, 0.0F, 1.0F - 1.0F / zoom);
}

// Keeps the playhead in view while it plays past the visible window.
void Follow(double fraction) {
    const float visible = 1.0F / zoom;
    if (fraction < scroll || fraction > scroll + visible) scroll = static_cast<float>(fraction) - visible * 0.1F;
    ClampView();
}
}

// "Exit replay", then "Confirm exit" for a few seconds; the second click
// leaves the replay world. Shared by the pause-screen dock and the editor.
Clock::time_point replayExitArmedAt{};

void ExitReplayButton(const char* id, float width, float height) {
    const auto now = Clock::now();
    const bool armed = now - replayExitArmedAt < std::chrono::seconds(3);
    if (armed) {
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(Canvas));
        ImGui::PushStyleColor(ImGuiCol_Button, ToVec4(Amber));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToVec4(Amber));
    }
    const bool clicked = Dock::TextButton(std::format("{}##{}", armed ? "Confirm exit" : "Exit replay", id).c_str(), width, height);
    if (armed) ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Leave the replay and go back to the menu");
    if (!clicked) return;
    if (armed) {
        replayExitArmedAt = {};
        ReplayInjector::Exit();
    } else {
        replayExitArmedAt = now;
    }
}

void DrawReplayDock(const ReplayInjector::Status& status) {
    const auto display = ImGui::GetIO().DisplaySize;
    const float tracks = 2.0F;
    const float height = Dock::ToolbarHeight + Dock::OverviewHeight + Dock::RulerHeight + Dock::TrackHeight * (tracks + 1.0F) + 8.0F;
    ImGui::SetNextWindowPos(ImVec2(0.0F, display.y - height), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(display.x, height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0F, 0.0F));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(Dock::Panel));
    ImGui::Begin("##replay-dock", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float width = display.x;
    const double length = static_cast<double>(std::max<std::uint64_t>(status.lengthMicros, 1));
    const auto shown = Dock::dragTarget.value_or(status.positionMicros);
    ImFont* font = CompactFont();

    // Toolbar.
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + Dock::ToolbarHeight), Dock::Header);
    draw->AddLine(ImVec2(origin.x, origin.y + Dock::ToolbarHeight), ImVec2(origin.x + width, origin.y + Dock::ToolbarHeight), Dock::Line);
    const float rowY = origin.y + 5.0F;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.0F, rowY));
    if (Dock::TextButton("-##zoom-out", 26.0F, 26.0F)) Dock::zoom /= 1.5F;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowY + 5.0F));
    ImGui::TextColored(ToVec4(Dock::Time), "%.1fx", Dock::zoom);
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + 2.0F, rowY));
    if (Dock::TextButton("+##zoom-in", 26.0F, 26.0F)) Dock::zoom *= 1.5F;
    ImGui::SameLine();
    if (Dock::TextButton("FIT", 40.0F, 26.0F)) {
        Dock::zoom = 1.0F;
        Dock::scroll = 0.0F;
    }

    // Transport, centred, with the current time to its left.
    constexpr float IconSize = 26.0F;
    const float transportWidth = IconSize * 5.0F + 4.0F * 4.0F;
    const float transportX = origin.x + (width - transportWidth) * 0.5F;
    const std::string current = Dock::Timecode(shown);
    const ImVec2 currentSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, current.c_str());
    draw->AddText(font, font->FontSize, ImVec2(transportX - currentSize.x - 16.0F, rowY + 13.0F - currentSize.y * 0.5F),
        Dock::Time, current.c_str());
    const auto seek = [&](double micros) {
        ReplayInjector::Seek(static_cast<std::uint64_t>(std::clamp(micros, 0.0, length)));
    };
    ImGui::SetCursorScreenPos(ImVec2(transportX, rowY));
    if (Dock::IconButton("##start", Dock::Icon::Start, IconSize)) seek(0.0);
    ImGui::SameLine();
    if (Dock::IconButton("##back", Dock::Icon::Back, IconSize)) seek(static_cast<double>(status.positionMicros) - Dock::StepMicros);
    ImGui::SameLine();
    if (Dock::IconButton("##play", status.paused ? Dock::Icon::Play : Dock::Icon::Pause, IconSize, true)) {
        ReplayInjector::SetPaused(!status.paused);
    }
    ImGui::SameLine();
    if (Dock::IconButton("##forward", Dock::Icon::Forward, IconSize)) seek(static_cast<double>(status.positionMicros) + Dock::StepMicros);
    ImGui::SameLine();
    if (Dock::IconButton("##end", Dock::Icon::End, IconSize)) seek(length);

    // Speed and total length, right-aligned.
    const std::string total = Dock::Timecode(status.lengthMicros);
    const ImVec2 totalSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, total.c_str());
    draw->AddText(font, font->FontSize, ImVec2(origin.x + width - totalSize.x - 14.0F, rowY + 13.0F - totalSize.y * 0.5F),
        Dock::Label, total.c_str());
    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - totalSize.x - 14.0F - 12.0F - 70.0F - 8.0F - 96.0F, rowY));
    ExitReplayButton("dock-exit", 96.0F, 26.0F);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - totalSize.x - 14.0F - 12.0F - 70.0F, rowY));
    if (Dock::TextButton(std::format("{}x speed##speed", status.speed).c_str(), 70.0F, 26.0F)) {
        const auto found = std::ranges::find(Dock::Speeds, status.speed);
        ReplayInjector::SetSpeed(found == Dock::Speeds.end() || found + 1 == Dock::Speeds.end() ? Dock::Speeds[0] : *(found + 1));
    }

    // Timeline geometry: everything right of the layer column.
    const float laneLeft = origin.x + Dock::LayersWidth;
    const float laneRight = origin.x + width - 12.0F;
    const float laneWidth = std::max(1.0F, laneRight - laneLeft);
    Dock::ClampView();
    const double visibleStart = static_cast<double>(Dock::scroll) * length;
    const double visibleLength = length / static_cast<double>(Dock::zoom);
    const auto toX = [&](double micros) { return laneLeft + static_cast<float>((micros - visibleStart) / visibleLength) * laneWidth; };
    const auto toMicros = [&](float x) { return visibleStart + static_cast<double>((x - laneLeft) / laneWidth) * visibleLength; };
    if (!Dock::dragTarget && !status.paused) Dock::Follow(static_cast<double>(status.positionMicros) / length);

    // Overview strip: the whole clip, with the visible window highlighted.
    const float overviewY = origin.y + Dock::ToolbarHeight + 4.0F;
    const ImVec2 overviewMin(laneLeft, overviewY);
    const ImVec2 overviewMax(laneRight, overviewY + Dock::OverviewHeight - 6.0F);
    draw->AddRectFilled(overviewMin, overviewMax, Dock::Lane, 3.0F);
    draw->AddRectFilled(ImVec2(laneLeft + laneWidth * Dock::scroll, overviewMin.y),
        ImVec2(laneLeft + laneWidth * (Dock::scroll + 1.0F / Dock::zoom), overviewMax.y), Dock::Line, 3.0F);
    const float overviewHead = laneLeft + laneWidth * static_cast<float>(static_cast<double>(shown) / length);
    draw->AddLine(ImVec2(overviewHead, overviewMin.y), ImVec2(overviewHead, overviewMax.y), Dock::Playhead, 2.0F);
    ImGui::SetCursorScreenPos(overviewMin);
    ImGui::InvisibleButton("##overview", ImVec2(laneWidth, overviewMax.y - overviewMin.y));
    if (ImGui::IsItemActive()) {
        const float fraction = std::clamp((ImGui::GetIO().MousePos.x - laneLeft) / laneWidth, 0.0F, 1.0F);
        Dock::dragTarget = static_cast<std::uint64_t>(fraction * length);
    }
    if (ImGui::IsItemDeactivated() && Dock::dragTarget) {
        ReplayInjector::Seek(*Dock::dragTarget);
        Dock::dragTarget.reset();
    }

    // Ruler.
    const float rulerY = origin.y + Dock::ToolbarHeight + Dock::OverviewHeight;
    draw->AddRectFilled(ImVec2(origin.x, rulerY), ImVec2(origin.x + width, rulerY + Dock::RulerHeight), Dock::Header);
    draw->AddText(font, font->FontSize, ImVec2(origin.x + 12.0F, rulerY + 6.0F), Dock::Label, "Layers");
    const auto step = Dock::TickStep(visibleLength, laneWidth);
    draw->PushClipRect(ImVec2(laneLeft - 1.0F, rulerY), ImVec2(laneRight + 40.0F, origin.y + height), true);
    for (auto t = static_cast<std::uint64_t>(visibleStart / static_cast<double>(step)) * step;
         static_cast<double>(t) <= visibleStart + visibleLength; t += step) {
        const float x = toX(static_cast<double>(t));
        draw->AddLine(ImVec2(x, rulerY + Dock::RulerHeight - 7.0F), ImVec2(x, rulerY + Dock::RulerHeight), Dock::Tick);
        const std::string label = Dock::Timecode(t);
        draw->AddText(font, font->FontSize * 0.9F, ImVec2(x + 3.0F, rulerY + 4.0F), Dock::Label, label.c_str());
        for (int minor = 1; minor < 4; ++minor) {
            const float mx = toX(static_cast<double>(t) + static_cast<double>(step) * minor / 4.0);
            draw->AddLine(ImVec2(mx, rulerY + Dock::RulerHeight - 4.0F), ImVec2(mx, rulerY + Dock::RulerHeight), Dock::Line);
        }
    }
    draw->PopClipRect();

    // Tracks: the recording player's span and the recording's marks.
    const float tracksY = rulerY + Dock::RulerHeight;
    const std::array<const char*, 2> names{"Player", "Marks"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const float y = tracksY + Dock::TrackHeight * static_cast<float>(index);
        draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + Dock::TrackHeight), index % 2 ? Dock::LaneAlt : Dock::Lane);
        draw->AddLine(ImVec2(origin.x, y + Dock::TrackHeight), ImVec2(origin.x + width, y + Dock::TrackHeight), Dock::Line);
        draw->AddCircleFilled(ImVec2(origin.x + 16.0F, y + Dock::TrackHeight * 0.5F), 3.0F, index == 0 ? Pink : Dock::Mark);
        draw->AddText(font, font->FontSize, ImVec2(origin.x + 26.0F, y + 5.0F), Text, names[index]);
    }
    draw->AddLine(ImVec2(laneLeft, rulerY), ImVec2(laneLeft, tracksY + Dock::TrackHeight * 2.0F), Dock::Line);
    draw->PushClipRect(ImVec2(laneLeft, tracksY), ImVec2(laneRight, tracksY + Dock::TrackHeight * 2.0F), true);
    draw->AddRectFilled(ImVec2(toX(0.0), tracksY + 6.0F), ImVec2(toX(length), tracksY + Dock::TrackHeight - 6.0F),
        WithAlpha(Pink, 0.45F), 4.0F);
    for (const auto& mark : status.marks) {
        const float x = toX(static_cast<double>(mark.positionMicros));
        const float cy = tracksY + Dock::TrackHeight * 1.5F;
        draw->AddQuadFilled(ImVec2(x, cy - 6.0F), ImVec2(x + 6.0F, cy), ImVec2(x, cy + 6.0F), ImVec2(x - 6.0F, cy), Dock::Mark);
        if (ImGui::IsMouseHoveringRect(ImVec2(x - 7.0F, cy - 7.0F), ImVec2(x + 7.0F, cy + 7.0F))) {
            ImGui::SetTooltip("%s  %s", mark.label.c_str(), Dock::Timecode(mark.positionMicros).c_str());
        }
    }
    draw->PopClipRect();

    // Scrubbing on the ruler and lanes; Ctrl+wheel zooms around the cursor.
    const float scrubHeight = Dock::RulerHeight + Dock::TrackHeight * 2.0F;
    ImGui::SetCursorScreenPos(ImVec2(laneLeft, rulerY));
    ImGui::InvisibleButton("##scrub", ImVec2(laneWidth, scrubHeight));
    const bool scrubHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActive()) {
        Dock::dragTarget = static_cast<std::uint64_t>(std::clamp(toMicros(ImGui::GetIO().MousePos.x), 0.0, length));
    }
    if (ImGui::IsItemDeactivated() && Dock::dragTarget) {
        ReplayInjector::Seek(*Dock::dragTarget);
        Dock::dragTarget.reset();
    }
    if (scrubHovered && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0F) {
        const double pivot = toMicros(ImGui::GetIO().MousePos.x) / length;
        const float pivotOffset = static_cast<float>(pivot) - Dock::scroll;
        const float before = Dock::zoom;
        Dock::zoom *= ImGui::GetIO().MouseWheel > 0.0F ? 1.25F : 0.8F;
        Dock::ClampView();
        Dock::scroll = static_cast<float>(pivot) - pivotOffset * before / Dock::zoom;
        Dock::ClampView();
    } else if (scrubHovered && ImGui::GetIO().MouseWheel != 0.0F) {
        Dock::scroll -= ImGui::GetIO().MouseWheel * 0.1F / Dock::zoom;
        Dock::ClampView();
    }

    // Playhead across ruler and lanes, with a handle on the ruler.
    const float headX = toX(static_cast<double>(shown));
    if (headX >= laneLeft - 1.0F && headX <= laneRight + 1.0F) {
        draw->AddLine(ImVec2(headX, rulerY), ImVec2(headX, tracksY + Dock::TrackHeight * 2.0F), Dock::Playhead, 2.0F);
        draw->AddTriangleFilled(ImVec2(headX - 6.0F, rulerY), ImVec2(headX + 6.0F, rulerY), ImVec2(headX, rulerY + 8.0F), Dock::Playhead);
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

// --- Replay editor -----------------------------------------------------------
// The full-screen editor that opens when a replay world is joined, laid out
// like Playback's: a title bar, an icon rail, a keyframe panel on the left,
// the game (the last raw frame, scaled) in a viewport on the right with the
// transport under it, and the timeline across the bottom. Keyframes are not
// implemented yet, so their controls are drawn disabled.
namespace Editor {
constexpr ImU32 Backdrop = IM_COL32(18, 13, 16, 255);
constexpr ImU32 Bar = Rail;
constexpr ImU32 PanelFill = Surface;
constexpr ImU32 Accent = Pink;
constexpr ImU32 Disabled = IM_COL32(98, 78, 89, 255);
constexpr float TitleHeight = 30.0F;
constexpr float RailWidth = 36.0F;
constexpr float TransportHeight = 40.0F;
constexpr float RowHeight = 28.0F;
constexpr float LayersWidth = 150.0F;

enum class Glyph { Keyframes, Person, Gear, Info, Plus, Pointer, Undo, Trash, Eye, Lock, ZoomIn, ZoomOut, Prev, Next, Close };

void DrawGlyph(ImDrawList* draw, Glyph glyph, ImVec2 c, float r, ImU32 color) {
    switch (glyph) {
    case Glyph::Keyframes:
        draw->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), color);
        break;
    case Glyph::Person:
        draw->AddCircle(ImVec2(c.x, c.y - r * 0.35F), r * 0.38F, color, 16, 1.6F);
        draw->PathArcTo(ImVec2(c.x, c.y + r * 0.95F), r * 0.75F, 3.14159F * 1.08F, 3.14159F * 1.92F, 16);
        draw->PathStroke(color, ImDrawFlags_None, 1.6F);
        break;
    case Glyph::Gear:
        for (int tooth = 0; tooth < 8; ++tooth) {
            const float angle = static_cast<float>(tooth) * 3.14159F / 4.0F;
            draw->AddLine(ImVec2(c.x + std::cos(angle) * r * 0.55F, c.y + std::sin(angle) * r * 0.55F),
                ImVec2(c.x + std::cos(angle) * r * 0.95F, c.y + std::sin(angle) * r * 0.95F), color, 2.4F);
        }
        draw->AddCircle(c, r * 0.6F, color, 16, 1.8F);
        draw->AddCircleFilled(c, r * 0.22F, color);
        break;
    case Glyph::Info:
        draw->AddCircle(c, r * 0.9F, color, 20, 1.5F);
        draw->AddCircleFilled(ImVec2(c.x, c.y - r * 0.4F), 1.3F, color);
        draw->AddLine(ImVec2(c.x, c.y - r * 0.1F), ImVec2(c.x, c.y + r * 0.5F), color, 1.6F);
        break;
    case Glyph::Plus:
        draw->AddLine(ImVec2(c.x - r * 0.7F, c.y), ImVec2(c.x + r * 0.7F, c.y), color, 1.8F);
        draw->AddLine(ImVec2(c.x, c.y - r * 0.7F), ImVec2(c.x, c.y + r * 0.7F), color, 1.8F);
        break;
    case Glyph::Pointer:
        draw->AddTriangleFilled(ImVec2(c.x - r * 0.5F, c.y - r * 0.8F), ImVec2(c.x - r * 0.5F, c.y + r * 0.5F),
            ImVec2(c.x + r * 0.5F, c.y + r * 0.1F), color);
        draw->AddLine(ImVec2(c.x - r * 0.1F, c.y + r * 0.1F), ImVec2(c.x + r * 0.3F, c.y + r * 0.8F), color, 1.8F);
        break;
    case Glyph::Undo:
        draw->PathArcTo(c, r * 0.6F, 3.14159F * 1.1F, 3.14159F * 2.6F, 16);
        draw->PathStroke(color, ImDrawFlags_None, 1.6F);
        draw->AddTriangleFilled(ImVec2(c.x - r * 0.95F, c.y - r * 0.45F), ImVec2(c.x - r * 0.25F, c.y - r * 0.45F),
            ImVec2(c.x - r * 0.6F, c.y + r * 0.1F), color);
        break;
    case Glyph::Trash:
        draw->AddRect(ImVec2(c.x - r * 0.5F, c.y - r * 0.45F), ImVec2(c.x + r * 0.5F, c.y + r * 0.85F), color, 1.5F, 0, 1.5F);
        draw->AddLine(ImVec2(c.x - r * 0.75F, c.y - r * 0.6F), ImVec2(c.x + r * 0.75F, c.y - r * 0.6F), color, 1.6F);
        draw->AddLine(ImVec2(c.x - r * 0.2F, c.y - r * 0.85F), ImVec2(c.x + r * 0.2F, c.y - r * 0.85F), color, 1.6F);
        break;
    case Glyph::Eye:
        draw->PathArcTo(ImVec2(c.x, c.y + r * 0.9F), r * 1.25F, 3.14159F * 1.23F, 3.14159F * 1.77F, 12);
        draw->PathArcTo(ImVec2(c.x, c.y - r * 0.9F), r * 1.25F, 3.14159F * 0.23F, 3.14159F * 0.77F, 12);
        draw->PathStroke(color, ImDrawFlags_Closed, 1.4F);
        draw->AddCircleFilled(c, r * 0.28F, color);
        break;
    case Glyph::Lock:
        draw->AddRectFilled(ImVec2(c.x - r * 0.55F, c.y - r * 0.1F), ImVec2(c.x + r * 0.55F, c.y + r * 0.75F), color, 1.5F);
        draw->PathArcTo(ImVec2(c.x, c.y - r * 0.15F), r * 0.36F, 3.14159F, 3.14159F * 2.0F, 10);
        draw->PathStroke(color, ImDrawFlags_None, 1.5F);
        break;
    case Glyph::ZoomIn:
    case Glyph::ZoomOut:
        draw->AddCircle(ImVec2(c.x - r * 0.15F, c.y - r * 0.15F), r * 0.55F, color, 16, 1.5F);
        draw->AddLine(ImVec2(c.x + r * 0.25F, c.y + r * 0.25F), ImVec2(c.x + r * 0.8F, c.y + r * 0.8F), color, 1.8F);
        draw->AddLine(ImVec2(c.x - r * 0.42F, c.y - r * 0.15F), ImVec2(c.x + r * 0.12F, c.y - r * 0.15F), color, 1.3F);
        if (glyph == Glyph::ZoomIn) {
            draw->AddLine(ImVec2(c.x - r * 0.15F, c.y - r * 0.42F), ImVec2(c.x - r * 0.15F, c.y + r * 0.12F), color, 1.3F);
        }
        break;
    case Glyph::Prev:
        draw->AddLine(ImVec2(c.x + r * 0.25F, c.y - r * 0.55F), ImVec2(c.x - r * 0.25F, c.y), color, 1.7F);
        draw->AddLine(ImVec2(c.x - r * 0.25F, c.y), ImVec2(c.x + r * 0.25F, c.y + r * 0.55F), color, 1.7F);
        break;
    case Glyph::Next:
        draw->AddLine(ImVec2(c.x - r * 0.25F, c.y - r * 0.55F), ImVec2(c.x + r * 0.25F, c.y), color, 1.7F);
        draw->AddLine(ImVec2(c.x + r * 0.25F, c.y), ImVec2(c.x - r * 0.25F, c.y + r * 0.55F), color, 1.7F);
        break;
    case Glyph::Close:
        draw->AddLine(ImVec2(c.x - r * 0.55F, c.y - r * 0.55F), ImVec2(c.x + r * 0.55F, c.y + r * 0.55F), color, 1.6F);
        draw->AddLine(ImVec2(c.x + r * 0.55F, c.y - r * 0.55F), ImVec2(c.x - r * 0.55F, c.y + r * 0.55F), color, 1.6F);
        break;
    }
}

// A square glyph button. Disabled buttons still show their tooltip.
bool GlyphButton(const char* id, Glyph glyph, ImVec2 position, float size, const char* tooltip,
    bool enabled = true, bool active = false) {
    ImGui::SetCursorScreenPos(position);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size)) && enabled;
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    if (active) draw->AddRectFilled(position, ImVec2(position.x + size, position.y + size), Accent, 5.0F);
    else if (hovered && enabled) draw->AddRectFilled(position, ImVec2(position.x + size, position.y + size), Dock::ButtonHover, 5.0F);
    const ImU32 color = active ? Canvas : !enabled ? Disabled : hovered ? Text : Dock::Label;
    DrawGlyph(draw, glyph, ImVec2(position.x + size * 0.5F, position.y + size * 0.5F), size * 0.3F, color);
    if (hovered && tooltip) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

}

// Camera keyframe editing, shared by the editor and the hotbar: an undo
// history of whole paths, the selected keyframe, a timeline drag, and a short
// notice when a keyframe could not be added.
namespace Keyframes {
constexpr std::size_t UndoDepth = 64;
constexpr auto NoticeDuration = std::chrono::milliseconds(2200);
std::vector<ReplayCameraPath::Path> undo;
std::optional<std::size_t> selected;
std::optional<std::size_t> dragging;
bool dragMoved{};
std::string notice;
Clock::time_point noticeAt{};

void Notify(std::string text) {
    notice = std::move(text);
    noticeAt = Clock::now();
}

void Commit(const ReplayCameraPath::Path& before, ReplayCameraPath::Path after) {
    if (before == after) return;
    undo.push_back(before);
    if (undo.size() > UndoDepth) undo.erase(undo.begin());
    ReplayInjector::SetCameraPath(std::move(after));
}

// Pins the current view to the playhead. False (with a notice) if it can't.
bool AddHere(const ReplayInjector::Status& status) {
    if (!status.playing) {
        Notify("The replay isn't playing yet");
        return false;
    }
    const auto view = WorldTelemetry::LatestView();
    if (!view || !view->yawDegrees || !view->pitchDegrees) {
        Notify("Can't tell where the camera is yet");
        return false;
    }
    const auto before = ReplayInjector::CameraPath();
    auto after = before;
    selected = after.Add({status.positionMicros, view->eye, *view->yawDegrees, *view->pitchDegrees});
    Notify(std::format("Keyframe {} of {} at {}", *selected + 1, after.Size(), Dock::Timecode(status.positionMicros)));
    Commit(before, std::move(after));
    return true;
}

void RemoveSelected() {
    const auto before = ReplayInjector::CameraPath();
    if (!selected || *selected >= before.Size()) {
        selected.reset();
        return;
    }
    auto after = before;
    after.Remove(*selected);
    selected.reset();
    Commit(before, std::move(after));
}

void Undo() {
    if (undo.empty()) return;
    auto previous = std::move(undo.back());
    undo.pop_back();
    selected.reset();
    ReplayInjector::SetCameraPath(std::move(previous));
}

// A new replay starts a new history.
void Reset() {
    undo.clear();
    selected.reset();
    dragging.reset();
    notice.clear();
}

bool NoticeShowing(Clock::time_point now) { return !notice.empty() && now - noticeAt < NoticeDuration; }

void SetEase(std::size_t index, ReplayCameraPath::Ease ease) {
    const auto before = ReplayInjector::CameraPath();
    auto after = before;
    after.SetEase(index, ease);
    Commit(before, std::move(after));
}
}

// Rendering a video: jump to the first keyframe and hold there while the
// world rebuilds, then record the game while the camera flies the path, and
// stop at the last keyframe. The recorder captures the game's frame before
// any Oinkster overlay, so nothing drawn here ends up in the video.
namespace Render {
enum class Stage { Idle, Preparing, Recording };
constexpr auto SettleTime = std::chrono::milliseconds(1500);
Stage stage{Stage::Idle};
Clock::time_point since{};
std::uint64_t startMicros{};
std::uint64_t endMicros{};
bool awaitingResult{};

std::filesystem::path VideoDirectory() {
    wchar_t localAppData[32768]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (length > 0 && length < std::size(localAppData)) {
        return std::filesystem::path(localAppData) / L"Oinkster" / L"Client" / L"videos";
    }
    return std::filesystem::temp_directory_path() / L"Oinkster" / L"videos";
}

// <replay name> <date> <time>.mp4
std::filesystem::path NextVideoFile() {
    const auto replay = ReplayInjector::WatchedReplay();
    const std::wstring stem = replay.empty() ? std::wstring(L"replay") : replay.stem().wstring();
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    (void)localtime_s(&local, &now);
    return VideoDirectory() / std::format(L"{} {:04}-{:02}-{:02} {:02}.{:02}.{:02}.mp4", stem, local.tm_year + 1900, local.tm_mon + 1,
        local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

bool Busy() noexcept { return stage != Stage::Idle; }

bool Begin(const ReplayCameraPath::Path& path) {
    if (path.Size() < 2 || stage != Stage::Idle || VideoRecorder::Recording() || VideoRecorder::Current().finishing) return false;
    startMicros = path.Keyframes().front().micros;
    endMicros = path.Keyframes().back().micros;
    ReplayInjector::SetPaused(true);
    ReplayInjector::SetSpeed(1.0F);
    ReplayInjector::Seek(startMicros);
    ReplayInjector::PlayCameraPath(true);
    stage = Stage::Preparing;
    since = Clock::now();
    Keyframes::Notify("Getting the first shot ready...");
    return true;
}

void Finish(bool stopPath) {
    if (stage == Stage::Recording) {
        VideoRecorder::Stop();
        awaitingResult = true;
    }
    if (stopPath) ReplayInjector::PlayCameraPath(false);
    ReplayInjector::SetPaused(true);
    stage = Stage::Idle;
}

// Once a frame.
void Update(const ReplayInjector::Status& status) {
    const auto now = Clock::now();
    if (stage != Stage::Idle && !status.running) {
        if (stage == Stage::Recording) {
            VideoRecorder::Stop();
            awaitingResult = true;
        }
        stage = Stage::Idle;
    } else if (stage == Stage::Preparing) {
        // Shift (or following a player) during the wait cancels.
        if (!status.cameraPathPlaying) {
            stage = Stage::Idle;
        } else if (now - since >= SettleTime) {
            std::string error;
            if (!VideoRecorder::Start(NextVideoFile(), error)) {
                Keyframes::Notify(error);
                Finish(true);
            } else {
                ReplayInjector::SetPaused(false);
                stage = Stage::Recording;
                since = now;
            }
        }
    } else if (stage == Stage::Recording) {
        if (!status.cameraPathPlaying) Finish(false);
        else if (status.positionMicros >= endMicros || status.positionMicros >= status.lengthMicros || !VideoRecorder::Recording()) Finish(true);
    }
    if (awaitingResult) {
        const auto recorded = VideoRecorder::Current();
        if (!recorded.recording && !recorded.finishing) {
            awaitingResult = false;
            Keyframes::Notify(recorded.error.empty()
                ? std::format("Saved \"{}\" to the videos folder", Narrow(recorded.file.filename().wstring()))
                : recorded.error);
        }
    }
}

// A small REC badge, top left. The recording never contains it.
void DrawBadge() {
    if (stage == Stage::Idle && !VideoRecorder::Current().finishing) return;
    auto* draw = ImGui::GetForegroundDrawList();
    ImFont* font = CompactFont();
    const auto recorded = VideoRecorder::Current();
    const std::string text = stage == Stage::Preparing ? std::string("Getting ready to record...")
        : stage == Stage::Recording ? std::format("REC  {}  /  {}   Shift to stop", Dock::Timecode(static_cast<std::uint64_t>(recorded.seconds * 1'000'000.0)),
            Dock::Timecode(endMicros - startMicros))
        : std::string("Saving the video...");
    const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, text.c_str());
    const ImVec2 min(14.0F, 14.0F);
    const ImVec2 max(min.x + size.x + 34.0F, min.y + size.y + 14.0F);
    draw->AddRectFilled(min, max, WithAlpha(Canvas, 0.88F), 8.0F);
    const bool blink = stage != Stage::Recording || std::fmod(std::chrono::duration<double>(Clock::now().time_since_epoch()).count(), 1.0) < 0.6;
    if (blink) draw->AddCircleFilled(ImVec2(min.x + 14.0F, (min.y + max.y) * 0.5F), 5.0F, stage == Stage::Recording ? IM_COL32(230, 70, 80, 255) : Amber);
    draw->AddText(font, font->FontSize, ImVec2(min.x + 26.0F, min.y + 7.0F), Text, text.c_str());
}
}

void DrawReplayEditor(const FrameInfo& frame, DrawResult& result) {
    if (!ReplayInjector::Running()) {
        result.closeRequested = true;
        return;
    }
    const auto status = ReplayInjector::CurrentStatus();
    const auto display = ImGui::GetIO().DisplaySize;
    const double length = static_cast<double>(std::max<std::uint64_t>(status.lengthMicros, 1));
    const auto shown = Dock::dragTarget.value_or(status.positionMicros);
    const auto seek = [&](double micros) {
        ReplayInjector::Seek(static_cast<std::uint64_t>(std::clamp(micros, 0.0, length)));
    };
    const auto cameraPath = ReplayInjector::CameraPath();
    const auto& keyframes = cameraPath.Keyframes();
    if (Keyframes::selected && *Keyframes::selected >= keyframes.size()) Keyframes::selected.reset();

    // Keyboard: Esc leaves the editor (the replay keeps playing), Space
    // plays/pauses, Left/Right step; N adds a keyframe, Delete removes the
    // selected one, Ctrl+Z undoes.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) result.closeRequested = true;
    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) (void)Keyframes::AddHere(status);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) Keyframes::RemoveSelected();
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) Keyframes::Undo();
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) ReplayInjector::SetPaused(!status.paused);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) seek(static_cast<double>(status.positionMicros) - Dock::StepMicros);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) seek(static_cast<double>(status.positionMicros) + Dock::StepMicros);

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(display, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0F, 0.0F));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(Editor::Backdrop));
    ImGui::Begin("##replay-editor", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    auto* draw = ImGui::GetWindowDrawList();
    ImFont* font = CompactFont();
    const float W = display.x;
    const float H = display.y;
    const auto textAt = [&](ImVec2 position, ImU32 color, const std::string& text, ImFont* with = nullptr) {
        ImFont* used = with ? with : font;
        draw->AddText(used, used->FontSize, position, color, text.c_str());
    };
    const auto textSize = [&](const std::string& text, ImFont* with = nullptr) {
        ImFont* used = with ? with : font;
        return used->CalcTextSizeA(used->FontSize, FLT_MAX, 0.0F, text.c_str());
    };

    // Geometry.
    const float timelineHeight = std::clamp(H * 0.2F, 150.0F, 260.0F);
    const float timelineTop = H - timelineHeight;
    const float leftX = Editor::RailWidth;
    const float leftWidth = std::clamp(W * 0.36F, 280.0F, 640.0F);
    const float rightX = leftX + leftWidth;
    const float bodyTop = Editor::TitleHeight;

    // Title bar.
    draw->AddRectFilled(ImVec2(0.0F, 0.0F), ImVec2(W, Editor::TitleHeight), Editor::Bar);
    draw->AddLine(ImVec2(0.0F, Editor::TitleHeight), ImVec2(W, Editor::TitleHeight), Dock::Line);
    const std::string keyframeCount = std::format("Keyframes: {}", keyframes.size());
    textAt(ImVec2(leftX + 10.0F, (Editor::TitleHeight - textSize(keyframeCount, BodyFont()).y) * 0.5F), Text, keyframeCount, BodyFont());
    const std::string title = "Replay Playback";
    const ImVec2 titleSize = textSize(title);
    textAt(ImVec2((W - titleSize.x) * 0.5F, (Editor::TitleHeight - titleSize.y) * 0.5F), Dock::Label, title);
    if (Editor::GlyphButton("##editor-close", Editor::Glyph::Close, ImVec2(W - 30.0F, 3.0F), 24.0F,
        "Close the editor (Esc). The replay keeps playing; K reopens it.")) {
        result.closeRequested = true;
    }
    const std::string speedText = status.paused ? "PAUSED" : std::format("{}x", status.speed);
    const ImVec2 speedSize = textSize(speedText);
    textAt(ImVec2(W - 42.0F - speedSize.x, (Editor::TitleHeight - speedSize.y) * 0.5F), status.paused ? Amber : Dock::Label, speedText);
    ImGui::SetCursorScreenPos(ImVec2(W - 42.0F - speedSize.x - 14.0F - 100.0F, 4.0F));
    ExitReplayButton("editor-exit", 100.0F, Editor::TitleHeight - 8.0F);
    {
        const bool rendering = Render::Busy();
        const bool canRender = rendering || keyframes.size() >= 2;
        ImGui::SetCursorScreenPos(ImVec2(W - 42.0F - speedSize.x - 14.0F - 100.0F - 8.0F - 112.0F, 4.0F));
        ImGui::BeginDisabled(!canRender);
        if (rendering ? Dock::TextButton("Stop render##render", 112.0F, Editor::TitleHeight - 8.0F)
                      : PrimaryButton("Render video##render", ImVec2(112.0F, Editor::TitleHeight - 8.0F))) {
            if (rendering) {
                Render::Finish(true);
            } else if (Render::Begin(cameraPath)) {
                // Watch it happen in the game; the video won't contain the editor anyway.
                result.closeRequested = true;
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", canRender
                ? "Fly the camera path from the first keyframe to the last and save it as an MP4 (videos folder)"
                : "Add two or more keyframes first");
        }
    }

    // Icon rail.
    draw->AddRectFilled(ImVec2(0.0F, bodyTop), ImVec2(Editor::RailWidth, H), Editor::Bar);
    draw->AddLine(ImVec2(Editor::RailWidth, bodyTop), ImVec2(Editor::RailWidth, H), Dock::Line);
    constexpr float RailButton = 26.0F;
    const float railX = (Editor::RailWidth - RailButton) * 0.5F;
    float railY = bodyTop + 8.0F;
    (void)Editor::GlyphButton("##rail-editor", Editor::Glyph::Keyframes, ImVec2(railX, railY), RailButton, "Replay editor", true, true);
    railY += RailButton + 6.0F;
    (void)Editor::GlyphButton("##rail-camera", Editor::Glyph::Person, ImVec2(railX, railY), RailButton,
        "Spectating the recording player", false);
    railY += RailButton + 6.0F;
    if (Editor::GlyphButton("##rail-settings", Editor::Glyph::Gear, ImVec2(railX, railY), RailButton, "Oinkster menu")) {
        result.openClickGui = true;
    }
    railY += RailButton + 6.0F;
    (void)Editor::GlyphButton("##rail-info", Editor::Glyph::Info, ImVec2(railX, railY), RailButton,
        "Space  play / pause\nLeft / Right  step 5 s\nN  add a camera keyframe here\nDelete  remove the selected keyframe\n"
        "Ctrl + Z  undo\nCtrl + wheel  zoom the timeline\nEsc  close the editor\nK  reopen it");
    float railBottom = H - 8.0F - RailButton;
    if (Editor::GlyphButton("##rail-trash", Editor::Glyph::Trash, ImVec2(railX, railBottom), RailButton,
        "Delete the selected keyframe (Delete)", Keyframes::selected.has_value())) {
        Keyframes::RemoveSelected();
    }
    railBottom -= RailButton + 6.0F;
    if (Editor::GlyphButton("##rail-undo", Editor::Glyph::Undo, ImVec2(railX, railBottom), RailButton, "Undo (Ctrl+Z)",
        !Keyframes::undo.empty())) {
        Keyframes::Undo();
    }
    railBottom -= RailButton + 6.0F;
    (void)Editor::GlyphButton("##rail-select", Editor::Glyph::Pointer, ImVec2(railX, railBottom), RailButton, "Select");
    railBottom -= RailButton + 6.0F;
    if (Editor::GlyphButton("##rail-add", Editor::Glyph::Plus, ImVec2(railX, railBottom), RailButton,
        "Add a camera keyframe here: the current view at the playhead (N)", status.playing)) {
        (void)Keyframes::AddHere(status);
    }

    // Left panel: the keyframe speed curve on top, playback settings below.
    draw->AddRectFilled(ImVec2(leftX, bodyTop), ImVec2(rightX, timelineTop), Editor::PanelFill);
    draw->AddLine(ImVec2(rightX, bodyTop), ImVec2(rightX, timelineTop), Dock::Line);
    const ImVec2 curveMin(leftX + 8.0F, bodyTop + 8.0F);
    const ImVec2 curveMax(rightX - 8.0F, bodyTop + std::max(140.0F, (timelineTop - bodyTop) * 0.55F));
    draw->AddRectFilled(curveMin, curveMax, Dock::Lane, 4.0F);
    for (int line = 1; line < 4; ++line) {
        const float y = curveMin.y + (curveMax.y - curveMin.y) * static_cast<float>(line) / 4.0F;
        draw->AddLine(ImVec2(curveMin.x + 6.0F, y), ImVec2(curveMax.x - 6.0F, y), WithAlpha(Dock::Line, 0.6F));
    }
    // The camera path seen from above (north up), its keyframes, and where
    // the path puts the camera at the playhead.
    textAt(ImVec2(curveMin.x + 10.0F, curveMin.y + 8.0F), Dock::Label, "CAMERA PATH  \xC2\xB7  TOP VIEW");
    {
        const ImVec2 plotMin(curveMin.x + 14.0F, curveMin.y + 18.0F + font->FontSize);
        const ImVec2 plotMax(curveMax.x - 14.0F, curveMax.y - 12.0F);
        if (keyframes.size() < 2) {
            const std::array<const char*, 3> hint{
                keyframes.empty() ? "No keyframes yet." : "One keyframe so far.",
                "Fly the camera to a spot and press + or N to pin it here.",
                "With two or more, press the lock to fly along them."};
            float hintY = (plotMin.y + plotMax.y) * 0.5F - font->FontSize * 1.8F;
            for (std::size_t line = 0; line < hint.size(); ++line) {
                const ImVec2 size = textSize(hint[line]);
                textAt(ImVec2((plotMin.x + plotMax.x - size.x) * 0.5F, hintY), line == 0 ? Secondary : Muted, hint[line]);
                hintY += font->FontSize + 6.0F;
            }
        } else {
            constexpr int Samples = 160;
            const auto first = keyframes.front().micros;
            const auto last = keyframes.back().micros;
            std::vector<Vec3> samples;
            samples.reserve(Samples + 1);
            for (int index = 0; index <= Samples; ++index) {
                const auto t = first + static_cast<std::uint64_t>(static_cast<double>(last - first) * index / Samples);
                if (const auto pose = cameraPath.Evaluate(t)) samples.push_back(pose->eye);
            }
            double minX = samples.front().x, maxX = minX, minZ = samples.front().z, maxZ = minZ;
            for (const auto& point : samples) {
                minX = std::min(minX, point.x);
                maxX = std::max(maxX, point.x);
                minZ = std::min(minZ, point.z);
                maxZ = std::max(maxZ, point.z);
            }
            const double spanX = std::max(maxX - minX, 4.0);
            const double spanZ = std::max(maxZ - minZ, 4.0);
            const double scale = std::min(static_cast<double>(plotMax.x - plotMin.x) / spanX,
                static_cast<double>(plotMax.y - plotMin.y) / spanZ);
            const double midX = (minX + maxX) * 0.5;
            const double midZ = (minZ + maxZ) * 0.5;
            const ImVec2 plotMid((plotMin.x + plotMax.x) * 0.5F, (plotMin.y + plotMax.y) * 0.5F);
            const auto map = [&](const Vec3& point) {
                return ImVec2(plotMid.x + static_cast<float>((point.x - midX) * scale), plotMid.y + static_cast<float>((point.z - midZ) * scale));
            };
            std::vector<ImVec2> line;
            line.reserve(samples.size());
            for (const auto& point : samples) line.push_back(map(point));
            draw->AddPolyline(line.data(), static_cast<int>(line.size()), WithAlpha(Pink, 0.75F), ImDrawFlags_None, 2.0F);
            for (std::size_t index = 0; index < keyframes.size(); ++index) {
                const ImVec2 at = map(keyframes[index].eye);
                const bool chosen = Keyframes::selected == index;
                ImGui::SetCursorScreenPos(ImVec2(at.x - 7.0F, at.y - 7.0F));
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::InvisibleButton("##plot-keyframe", ImVec2(14.0F, 14.0F))) {
                    Keyframes::selected = index;
                    seek(static_cast<double>(keyframes[index].micros));
                }
                const bool hovered = ImGui::IsItemHovered();
                if (hovered) ImGui::SetTooltip("Keyframe %zu  %s", index + 1, Dock::Timecode(keyframes[index].micros).c_str());
                ImGui::PopID();
                const float radius = chosen || hovered ? 5.5F : 4.0F;
                draw->AddQuadFilled(ImVec2(at.x, at.y - radius), ImVec2(at.x + radius, at.y), ImVec2(at.x, at.y + radius),
                    ImVec2(at.x - radius, at.y), chosen ? Pink : Secondary);
            }
            if (const auto pose = cameraPath.Evaluate(shown)) {
                // Bedrock yaw: 0 faces +Z (south, down here), 90 faces -X.
                const ImVec2 at = map(pose->eye);
                const double yaw = static_cast<double>(pose->yaw) * 3.14159265358979 / 180.0;
                const ImVec2 facing(static_cast<float>(-std::sin(yaw)), static_cast<float>(std::cos(yaw)));
                draw->AddLine(at, ImVec2(at.x + facing.x * 16.0F, at.y + facing.y * 16.0F), Text, 1.6F);
                draw->AddCircleFilled(at, 4.0F, Text);
            }
        }
    }

    float panelY = curveMax.y + 14.0F;
    textAt(ImVec2(leftX + 12.0F, panelY), Dock::Label, "PLAYBACK SPEED");
    panelY += font->FontSize + 8.0F;
    {
        const float available = leftWidth - 24.0F;
        const float buttonWidth = (available - 4.0F * static_cast<float>(Dock::Speeds.size() - 1)) / static_cast<float>(Dock::Speeds.size());
        for (std::size_t index = 0; index < Dock::Speeds.size(); ++index) {
            const float speed = Dock::Speeds[index];
            const ImVec2 position(leftX + 12.0F + (buttonWidth + 4.0F) * static_cast<float>(index), panelY);
            ImGui::SetCursorScreenPos(position);
            const bool selected = status.speed == speed;
            ImGui::PushID(static_cast<int>(index));
            const bool clicked = ImGui::InvisibleButton("##speed", ImVec2(buttonWidth, 26.0F));
            ImGui::PopID();
            const bool hovered = ImGui::IsItemHovered();
            draw->AddRectFilled(position, ImVec2(position.x + buttonWidth, position.y + 26.0F),
                selected ? Editor::Accent : hovered ? Dock::ButtonHover : Dock::Button, 5.0F);
            const std::string label = std::format("{}x", speed);
            const ImVec2 size = textSize(label);
            textAt(ImVec2(position.x + (buttonWidth - size.x) * 0.5F, position.y + 13.0F - size.y * 0.5F),
                selected ? Canvas : hovered ? Text : Secondary, label);
            if (clicked) ReplayInjector::SetSpeed(speed);
        }
    }
    panelY += 40.0F;
    // How the camera leaves the selected keyframe.
    if (Keyframes::selected && *Keyframes::selected < keyframes.size()) {
        const auto index = *Keyframes::selected;
        textAt(ImVec2(leftX + 12.0F, panelY), Dock::Label, std::format("KEYFRAME {}  \xC2\xB7  {}  \xC2\xB7  MOVE TO THE NEXT", index + 1,
            Dock::Timecode(keyframes[index].micros)));
        panelY += font->FontSize + 8.0F;
        using ReplayCameraPath::Ease;
        constexpr std::array<std::pair<Ease, const char*>, 3> Options{{
            {Ease::Smooth, "Smooth"}, {Ease::EaseInOut, "Ease in/out"}, {Ease::Cut, "Hold, then cut"}}};
        constexpr std::array<const char*, 3> Help{"An even glide through the keyframes",
            "Slows to a stop at this keyframe and the next", "Stays on this shot, then jumps to the next"};
        const float available = leftWidth - 24.0F;
        const float buttonWidth = (available - 8.0F) / 3.0F;
        const bool last = index + 1 >= keyframes.size();
        for (std::size_t option = 0; option < Options.size(); ++option) {
            const ImVec2 position(leftX + 12.0F + (buttonWidth + 4.0F) * static_cast<float>(option), panelY);
            ImGui::SetCursorScreenPos(position);
            ImGui::PushID(static_cast<int>(option));
            const bool clicked = ImGui::InvisibleButton("##ease", ImVec2(buttonWidth, 26.0F)) && !last;
            ImGui::PopID();
            const bool hovered = ImGui::IsItemHovered();
            const bool chosen = keyframes[index].ease == Options[option].first;
            draw->AddRectFilled(position, ImVec2(position.x + buttonWidth, position.y + 26.0F),
                chosen && !last ? Editor::Accent : hovered && !last ? Dock::ButtonHover : Dock::Button, 5.0F);
            const ImVec2 size = textSize(Options[option].second);
            textAt(ImVec2(position.x + (buttonWidth - size.x) * 0.5F, position.y + 13.0F - size.y * 0.5F),
                last ? Editor::Disabled : chosen ? Canvas : hovered ? Text : Secondary, Options[option].second);
            if (hovered) ImGui::SetTooltip("%s", last ? "The last keyframe ends the path" : Help[option]);
            if (clicked && !chosen) Keyframes::SetEase(index, Options[option].first);
        }
        panelY += 40.0F;
    }
    textAt(ImVec2(leftX + 12.0F, panelY), Dock::Label, "RECORDING");
    panelY += font->FontSize + 8.0F;
    textAt(ImVec2(leftX + 12.0F, panelY), Secondary, std::format("Length  {}", Dock::Timecode(status.lengthMicros)));
    panelY += font->FontSize + 4.0F;
    textAt(ImVec2(leftX + 12.0F, panelY), Secondary, std::format("Marks  {}", status.marks.size()));

    panelY += font->FontSize + 18.0F;
    textAt(ImVec2(leftX + 12.0F, panelY), Dock::Label, "PLAYERS");
    panelY += font->FontSize + 8.0F;
    const auto players = ReplayInjector::Players();
    if (players.empty()) {
        textAt(ImVec2(leftX + 12.0F, panelY), Muted, "No players have appeared yet.");
    }
    for (const auto& player : players) {
        if (panelY + 26.0F > timelineTop - 8.0F) break;
        const bool following = status.spectating && *status.spectating == player.name;
        const std::string label = player.recorder ? player.name + "  (recording)" : player.name;
        textAt(ImVec2(leftX + 12.0F, panelY + 13.0F - font->FontSize * 0.5F), following ? Pink : Text, label);
        ImGui::PushID(static_cast<int>(player.runtimeId & 0x7FFFFFFF));
        ImGui::SetCursorScreenPos(ImVec2(rightX - 12.0F - 64.0F - 6.0F - 44.0F, panelY));
        if (Dock::TextButton("TP", 44.0F, 26.0F)) ReplayInjector::TeleportTo(player.runtimeId);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Teleport to %s", player.name.c_str());
        ImGui::SetCursorScreenPos(ImVec2(rightX - 12.0F - 64.0F, panelY));
        if (Dock::TextButton(following ? "Stop" : "View", 64.0F, 26.0F)) {
            ReplayInjector::Spectate(following ? std::nullopt : std::optional<std::uint64_t>(player.runtimeId));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(following ? "Stop following" : "See through %s's eyes", player.name.c_str());
        ImGui::PopID();
        panelY += 32.0F;
    }

    // Viewport: the game frame scaled to fit, transport underneath.
    const ImVec2 areaMin(rightX + 14.0F, bodyTop + 14.0F);
    const ImVec2 areaMax(W - 14.0F, timelineTop - Editor::TransportHeight - 6.0F);
    const float aspect = display.y > 0.0F ? display.x / display.y : 16.0F / 9.0F;
    float viewWidth = std::max(1.0F, areaMax.x - areaMin.x);
    float viewHeight = viewWidth / aspect;
    if (viewHeight > areaMax.y - areaMin.y) {
        viewHeight = std::max(1.0F, areaMax.y - areaMin.y);
        viewWidth = viewHeight * aspect;
    }
    const ImVec2 viewMin(areaMin.x + (areaMax.x - areaMin.x - viewWidth) * 0.5F, areaMin.y + (areaMax.y - areaMin.y - viewHeight) * 0.5F);
    const ImVec2 viewMax(viewMin.x + viewWidth, viewMin.y + viewHeight);
    draw->AddRectFilled(viewMin, viewMax, IM_COL32(0, 0, 0, 255));
    if (frame.gameFrameTexture) {
        draw->AddImage(reinterpret_cast<ImTextureID>(*frame.gameFrameTexture), viewMin, viewMax);
    }
    if (!status.playing || !frame.gameFrameTexture) {
        const std::string waiting = status.playing ? std::string("Waiting for the game...") : status.phaseText;
        const ImVec2 size = textSize(waiting);
        textAt(ImVec2((viewMin.x + viewMax.x - size.x) * 0.5F, (viewMin.y + viewMax.y - size.y) * 0.5F), Dock::Label, waiting);
    }
    draw->AddRect(viewMin, viewMax, Dock::Line);
    if (Keyframes::NoticeShowing(Clock::now())) {
        const ImVec2 size = textSize(Keyframes::notice);
        const ImVec2 at(viewMax.x - size.x - 12.0F, viewMin.y + 8.0F);
        draw->AddRectFilled(ImVec2(at.x - 8.0F, at.y - 4.0F), ImVec2(at.x + size.x + 8.0F, at.y + size.y + 4.0F), WithAlpha(Canvas, 0.85F), 6.0F);
        textAt(at, Pink, Keyframes::notice);
    }
    if (const auto view = WorldTelemetry::LatestView(); view && frame.gameFrameTexture) {
        const std::string where = status.spectating
            ? std::format("Viewing {}", *status.spectating)
            : std::format("Position: {:.0f}, {:.0f}, {:.0f}", view->eye.x, view->eye.y - 1.62, view->eye.z);
        textAt(ImVec2(viewMin.x + 11.0F, viewMin.y + 9.0F), IM_COL32(0, 0, 0, 170), where);
        textAt(ImVec2(viewMin.x + 10.0F, viewMin.y + 8.0F), Text, where);
    }

    const float transportY = viewMax.y + (Editor::TransportHeight - 26.0F) * 0.5F + 2.0F;
    const std::string current = Dock::Timecode(shown);
    textAt(ImVec2(viewMin.x, transportY + 13.0F - textSize(current).y * 0.5F), Dock::Time, current);
    const std::string total = Dock::Timecode(status.lengthMicros);
    textAt(ImVec2(viewMax.x - textSize(total).x, transportY + 13.0F - textSize(total).y * 0.5F), Dock::Label, total);
    {
        constexpr float IconSize = 26.0F;
        constexpr float Gap = 4.0F;
        const float transportWidth = IconSize * 8.0F + Gap * 7.0F + 8.0F;
        float x = (viewMin.x + viewMax.x - transportWidth) * 0.5F;
        const auto place = [&] { ImGui::SetCursorScreenPos(ImVec2(x, transportY)); x += IconSize + Gap; };
        place();
        if (Dock::IconButton("##ed-start", Dock::Icon::Start, IconSize)) seek(0.0);
        place();
        if (Dock::IconButton("##ed-back", Dock::Icon::Back, IconSize)) seek(static_cast<double>(status.positionMicros) - Dock::StepMicros);
        place();
        if (Dock::IconButton("##ed-play", status.paused ? Dock::Icon::Play : Dock::Icon::Pause, IconSize, true)) {
            ReplayInjector::SetPaused(!status.paused);
        }
        place();
        if (Dock::IconButton("##ed-forward", Dock::Icon::Forward, IconSize)) seek(static_cast<double>(status.positionMicros) + Dock::StepMicros);
        place();
        if (Dock::IconButton("##ed-end", Dock::Icon::End, IconSize)) seek(length);
        x += 8.0F;
        // Previous / next recording mark.
        const auto previousMark = std::ranges::find_if(status.marks.rbegin(), status.marks.rend(),
            [&](const auto& mark) { return mark.positionMicros + 500'000 < status.positionMicros; });
        const auto nextMark = std::ranges::find_if(status.marks,
            [&](const auto& mark) { return mark.positionMicros > status.positionMicros + 500'000; });
        if (Editor::GlyphButton("##ed-prev-mark", Editor::Glyph::Prev, ImVec2(x, transportY), IconSize, "Previous mark",
            previousMark != status.marks.rend())) {
            seek(static_cast<double>(previousMark->positionMicros));
        }
        x += IconSize + Gap;
        if (Editor::GlyphButton("##ed-next-mark", Editor::Glyph::Next, ImVec2(x, transportY), IconSize, "Next mark",
            nextMark != status.marks.end())) {
            seek(static_cast<double>(nextMark->positionMicros));
        }
        x += IconSize + Gap;
        const bool canFly = keyframes.size() >= 2 || status.cameraPathPlaying;
        if (Editor::GlyphButton("##ed-lock", Editor::Glyph::Lock, ImVec2(x, transportY), IconSize,
            status.cameraPathPlaying ? "Stop flying along the camera path"
                : canFly ? "Fly the camera along the keyframes" : "Add two or more keyframes to fly along them",
            canFly, status.cameraPathPlaying)) {
            ReplayInjector::PlayCameraPath(!status.cameraPathPlaying);
        }
    }

    // Timeline.
    draw->AddRectFilled(ImVec2(leftX, timelineTop), ImVec2(W, H), Editor::Bar);
    draw->AddLine(ImVec2(leftX, timelineTop), ImVec2(W, timelineTop), Dock::Line);
    const float columnRight = leftX + Editor::LayersWidth;
    const float laneLeft = columnRight + 6.0F;
    const float laneRight = W - 12.0F;
    const float laneWidth = std::max(1.0F, laneRight - laneLeft);

    // Row 1: zoom controls in the layer column, the overview strip beside them.
    const float row1 = timelineTop + 2.0F;
    if (Editor::GlyphButton("##zoom-out", Editor::Glyph::ZoomOut, ImVec2(leftX + 6.0F, row1 + 2.0F), 24.0F, "Zoom out")) Dock::zoom /= 1.5F;
    const std::string zoomText = std::format("{:.1f}x", Dock::zoom);
    textAt(ImVec2(leftX + 34.0F, row1 + 14.0F - textSize(zoomText).y * 0.5F), Dock::Time, zoomText);
    if (Editor::GlyphButton("##zoom-in", Editor::Glyph::ZoomIn, ImVec2(leftX + 40.0F + textSize(zoomText).x, row1 + 2.0F), 24.0F,
        "Zoom in")) {
        Dock::zoom *= 1.5F;
    }
    ImGui::SetCursorScreenPos(ImVec2(columnRight - 40.0F, row1 + 3.0F));
    if (Dock::TextButton("FIT##timeline-fit", 34.0F, 22.0F)) {
        Dock::zoom = 1.0F;
        Dock::scroll = 0.0F;
    }
    Dock::ClampView();
    const double visibleStart = static_cast<double>(Dock::scroll) * length;
    const double visibleLength = length / static_cast<double>(Dock::zoom);
    const auto toX = [&](double micros) { return laneLeft + static_cast<float>((micros - visibleStart) / visibleLength) * laneWidth; };
    const auto toMicros = [&](float x) { return visibleStart + static_cast<double>((x - laneLeft) / laneWidth) * visibleLength; };
    if (!Dock::dragTarget && !status.paused) Dock::Follow(static_cast<double>(status.positionMicros) / length);

    const ImVec2 overviewMin(laneLeft, row1 + 6.0F);
    const ImVec2 overviewMax(laneRight, row1 + Editor::RowHeight - 8.0F);
    draw->AddRectFilled(overviewMin, overviewMax, Dock::Lane, 3.0F);
    draw->AddRect(ImVec2(laneLeft + laneWidth * Dock::scroll, overviewMin.y),
        ImVec2(laneLeft + laneWidth * (Dock::scroll + 1.0F / Dock::zoom), overviewMax.y), Dock::Tick, 3.0F);
    for (const auto& mark : status.marks) {
        const float x = laneLeft + laneWidth * static_cast<float>(static_cast<double>(mark.positionMicros) / length);
        draw->AddLine(ImVec2(x, overviewMin.y + 2.0F), ImVec2(x, overviewMax.y - 2.0F), Dock::Mark);
    }
    const float overviewHead = laneLeft + laneWidth * static_cast<float>(static_cast<double>(shown) / length);
    draw->AddLine(ImVec2(overviewHead, overviewMin.y - 2.0F), ImVec2(overviewHead, overviewMax.y + 2.0F), Dock::Playhead, 2.0F);
    ImGui::SetCursorScreenPos(overviewMin);
    ImGui::InvisibleButton("##ed-overview", ImVec2(laneWidth, std::max(1.0F, overviewMax.y - overviewMin.y)));
    if (ImGui::IsItemActive()) {
        const float fraction = std::clamp((ImGui::GetIO().MousePos.x - laneLeft) / laneWidth, 0.0F, 1.0F);
        Dock::dragTarget = static_cast<std::uint64_t>(fraction * length);
    }
    if (ImGui::IsItemDeactivated() && Dock::dragTarget) {
        ReplayInjector::Seek(*Dock::dragTarget);
        Dock::dragTarget.reset();
    }

    // Row 2: "Layers" and the ruler.
    const float rulerY = timelineTop + Editor::RowHeight;
    draw->AddRectFilled(ImVec2(leftX, rulerY), ImVec2(W, rulerY + Dock::RulerHeight), Dock::Header);
    draw->AddLine(ImVec2(leftX, rulerY), ImVec2(W, rulerY), Dock::Line);
    textAt(ImVec2(leftX + 10.0F, rulerY + Dock::RulerHeight * 0.5F - font->FontSize * 0.5F), Dock::Label, "Layers");
    const float lanesBottom = H;
    const auto step = Dock::TickStep(visibleLength, laneWidth);
    draw->PushClipRect(ImVec2(laneLeft - 1.0F, rulerY), ImVec2(laneRight + 40.0F, lanesBottom), true);
    for (auto t = static_cast<std::uint64_t>(visibleStart / static_cast<double>(step)) * step;
         static_cast<double>(t) <= visibleStart + visibleLength; t += step) {
        const float x = toX(static_cast<double>(t));
        draw->AddLine(ImVec2(x, rulerY + Dock::RulerHeight - 7.0F), ImVec2(x, rulerY + Dock::RulerHeight), Dock::Tick);
        draw->AddText(font, font->FontSize * 0.9F, ImVec2(x + 3.0F, rulerY + 4.0F), Dock::Label, Dock::Timecode(t).c_str());
        for (int minor = 1; minor < 4; ++minor) {
            const float mx = toX(static_cast<double>(t) + static_cast<double>(step) * minor / 4.0);
            draw->AddLine(ImVec2(mx, rulerY + Dock::RulerHeight - 4.0F), ImVec2(mx, rulerY + Dock::RulerHeight), Dock::Line);
        }
    }
    draw->PopClipRect();

    // Tracks: the camera keyframes and the recording's marks.
    const float tracksY = rulerY + Dock::RulerHeight;
    const std::array<std::string, 2> layers{std::format("Camera \xC2\xB7 {}", keyframes.size()),
        std::format("Marks \xC2\xB7 {}", status.marks.size())};
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const float y = tracksY + Dock::TrackHeight * static_cast<float>(index);
        draw->AddRectFilled(ImVec2(leftX, y), ImVec2(W, y + Dock::TrackHeight), index % 2 ? Dock::LaneAlt : Dock::Lane);
        draw->AddLine(ImVec2(leftX, y + Dock::TrackHeight), ImVec2(W, y + Dock::TrackHeight), Dock::Line);
        const ImU32 layerColor = index != 0 ? Dock::Mark : status.cameraPathPlaying ? Pink
            : keyframes.size() >= 2 ? Dock::Label : Editor::Disabled;
        Editor::DrawGlyph(draw, index == 0 ? Editor::Glyph::Lock : Editor::Glyph::Keyframes,
            ImVec2(leftX + 14.0F, y + Dock::TrackHeight * 0.5F), 6.0F, layerColor);
        textAt(ImVec2(leftX + 26.0F, y + Dock::TrackHeight * 0.5F - font->FontSize * 0.5F), Text, layers[index]);
        Editor::DrawGlyph(draw, Editor::Glyph::Eye, ImVec2(columnRight - 16.0F, y + Dock::TrackHeight * 0.5F), 6.5F, Dock::Label);
    }
    draw->AddLine(ImVec2(columnRight, rulerY), ImVec2(columnRight, lanesBottom), Dock::Line);
    draw->PushClipRect(ImVec2(laneLeft, tracksY), ImVec2(laneRight, lanesBottom), true);
    const float keyY = tracksY + Dock::TrackHeight * 0.5F;
    if (keyframes.size() >= 2) {
        draw->AddRectFilled(ImVec2(toX(static_cast<double>(keyframes.front().micros)), keyY - 2.0F),
            ImVec2(toX(static_cast<double>(keyframes.back().micros)), keyY + 2.0F), WithAlpha(Pink, status.cameraPathPlaying ? 0.6F : 0.3F), 2.0F);
    }
    for (std::size_t index = 0; index < keyframes.size(); ++index) {
        const bool dragged = Keyframes::dragging == index && Keyframes::dragMoved;
        const float x = dragged ? std::clamp(ImGui::GetIO().MousePos.x, laneLeft, laneRight) : toX(static_cast<double>(keyframes[index].micros));
        const bool chosen = Keyframes::selected == index;
        const float r = chosen ? 7.0F : 6.0F;
        draw->AddQuadFilled(ImVec2(x, keyY - r), ImVec2(x + r, keyY), ImVec2(x, keyY + r), ImVec2(x - r, keyY), chosen ? Pink : Secondary);
        draw->AddQuad(ImVec2(x, keyY - r), ImVec2(x + r, keyY), ImVec2(x, keyY + r), ImVec2(x - r, keyY), Canvas, 1.2F);
    }
    for (const auto& mark : status.marks) {
        const float x = toX(static_cast<double>(mark.positionMicros));
        const float cy = tracksY + Dock::TrackHeight * 1.5F;
        draw->AddQuadFilled(ImVec2(x, cy - 6.0F), ImVec2(x + 6.0F, cy), ImVec2(x, cy + 6.0F), ImVec2(x - 6.0F, cy), Dock::Mark);
        if (ImGui::IsMouseHoveringRect(ImVec2(x - 7.0F, cy - 7.0F), ImVec2(x + 7.0F, cy + 7.0F))) {
            ImGui::SetTooltip("%s  %s", mark.label.c_str(), Dock::Timecode(mark.positionMicros).c_str());
        }
    }
    draw->PopClipRect();

    // Scrubbing on the ruler and lanes; Ctrl+wheel zooms around the cursor.
    ImGui::SetCursorScreenPos(ImVec2(laneLeft, rulerY));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##ed-scrub", ImVec2(laneWidth, std::max(1.0F, lanesBottom - rulerY)));
    const bool scrubHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActive()) {
        Dock::dragTarget = static_cast<std::uint64_t>(std::clamp(toMicros(ImGui::GetIO().MousePos.x), 0.0, length));
    }
    if (ImGui::IsItemDeactivated() && Dock::dragTarget) {
        ReplayInjector::Seek(*Dock::dragTarget);
        Dock::dragTarget.reset();
    }
    // Keyframes sit over the scrub area: a click selects one and jumps to it,
    // a drag moves it in time.
    for (std::size_t index = 0; index < keyframes.size(); ++index) {
        const float x = toX(static_cast<double>(keyframes[index].micros));
        if (x < laneLeft - 8.0F || x > laneRight + 8.0F) continue;
        ImGui::SetCursorScreenPos(ImVec2(x - 8.0F, keyY - 8.0F));
        ImGui::PushID(static_cast<int>(index));
        ImGui::InvisibleButton("##keyframe", ImVec2(16.0F, 16.0F));
        if (ImGui::IsItemActivated()) {
            Keyframes::dragging = index;
            Keyframes::dragMoved = false;
            Keyframes::selected = index;
        }
        if (ImGui::IsItemActive() && std::abs(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x) > 3.0F) Keyframes::dragMoved = true;
        if (ImGui::IsItemDeactivated() && Keyframes::dragging == index) {
            if (Keyframes::dragMoved) {
                const auto target = static_cast<std::uint64_t>(std::clamp(toMicros(ImGui::GetIO().MousePos.x), 0.0, length));
                auto after = cameraPath;
                if (const auto moved = after.Retime(index, target)) {
                    Keyframes::selected = *moved;
                    Keyframes::Commit(cameraPath, std::move(after));
                } else {
                    Keyframes::Notify("Another keyframe is already there");
                }
            } else {
                seek(static_cast<double>(keyframes[index].micros));
            }
            Keyframes::dragging.reset();
        }
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
            ImGui::SetTooltip("Keyframe %zu  %s\nDrag to move it in time; Delete removes it", index + 1,
                Dock::Timecode(keyframes[index].micros).c_str());
        }
        ImGui::PopID();
    }
    if (scrubHovered && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0F) {
        const double pivot = toMicros(ImGui::GetIO().MousePos.x) / length;
        const float pivotOffset = static_cast<float>(pivot) - Dock::scroll;
        const float before = Dock::zoom;
        Dock::zoom *= ImGui::GetIO().MouseWheel > 0.0F ? 1.25F : 0.8F;
        Dock::ClampView();
        Dock::scroll = static_cast<float>(pivot) - pivotOffset * before / Dock::zoom;
        Dock::ClampView();
    } else if (scrubHovered && ImGui::GetIO().MouseWheel != 0.0F) {
        Dock::scroll -= ImGui::GetIO().MouseWheel * 0.1F / Dock::zoom;
        Dock::ClampView();
    }

    // Playhead across the ruler and lanes.
    const float headX = toX(static_cast<double>(shown));
    if (headX >= laneLeft - 1.0F && headX <= laneRight + 1.0F) {
        draw->AddLine(ImVec2(headX, rulerY), ImVec2(headX, lanesBottom), Dock::Playhead, 2.0F);
        draw->AddTriangleFilled(ImVec2(headX - 6.0F, rulerY), ImVec2(headX + 6.0F, rulerY), ImVec2(headX, rulerY + 8.0F), Dock::Playhead);
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

// The replay hotbar, shown while a replay is watched with the camera: like
// Minecraft's hotbar, the scroll wheel picks a slot and a left click uses it
// (both taken from the game by the mouse feed hook), so the replay can be
// paused, skipped and sped up without freeing the cursor. A right click on a
// player follows their view, as in Java's spectator mode.
namespace Hotbar {
enum class Slot { Start, Back, PlayPause, Forward, Keyframe, Speed, Teleport, Editor, Exit };
constexpr std::array<Slot, 9> Slots{Slot::Start, Slot::Back, Slot::PlayPause, Slot::Forward, Slot::Keyframe, Slot::Speed,
    Slot::Teleport, Slot::Editor, Slot::Exit};
constexpr auto ExitConfirmWindow = std::chrono::seconds(3);
constexpr auto NoticeDuration = std::chrono::milliseconds(1800);
int selected = 2;
// With more than one player, Teleport opens a list to pick from; while it is
// open the wheel and clicks drive the list instead of the slots. The last
// row is Cancel.
bool pickerOpen{};
int pickerIndex{};
// Exit needs a second click within ExitConfirmWindow.
Clock::time_point exitArmedAt{};
std::string notice;
Clock::time_point noticeAt{};
bool shiftWasDown{};
constexpr auto CleanShotDelay = std::chrono::milliseconds(2500);
bool wasFlying{};
Clock::time_point flyingSince{};

float NextSpeed(float speed) {
    const auto found = std::ranges::find(Dock::Speeds, speed);
    return found == Dock::Speeds.end() || found + 1 == Dock::Speeds.end() ? Dock::Speeds[0] : *(found + 1);
}

bool ExitArmed(Clock::time_point now) { return now - exitArmedAt < ExitConfirmWindow; }


std::string Name(Slot slot, const ReplayInjector::Status& status, const std::vector<ReplayInjector::Player>& players,
    Clock::time_point now) {
    switch (slot) {
    case Slot::Start: return "Jump to start";
    case Slot::Back: return "Back 5 seconds";
    case Slot::PlayPause: return status.paused ? "Play" : "Pause";
    case Slot::Forward: return "Forward 5 seconds";
    case Slot::Keyframe:
        return status.cameraKeyframes == 0 ? std::string("Add a camera keyframe here")
            : std::format("Add a camera keyframe here ({} so far)", status.cameraKeyframes);
    case Slot::Speed: return std::format("Speed {}x  (click for {}x)", status.speed, NextSpeed(status.speed));
    case Slot::Teleport:
        if (players.empty()) return "Teleport (no players yet)";
        if (players.size() == 1) return std::format("Teleport to {}", players.front().name);
        return std::format("Teleport to a player... ({})", players.size());
    case Slot::Editor: return "Open the replay editor";
    case Slot::Exit: return ExitArmed(now) ? "Click again to exit the replay" : "Exit the replay";
    }
    return {};
}

void Use(Slot slot, const ReplayInjector::Status& status, const std::vector<ReplayInjector::Player>& players,
    Clock::time_point now, DrawResult& result) {
    const double length = static_cast<double>(status.lengthMicros);
    const auto seek = [&](double micros) {
        ReplayInjector::Seek(static_cast<std::uint64_t>(std::clamp(micros, 0.0, length)));
    };
    switch (slot) {
    case Slot::Start: seek(0.0); break;
    case Slot::Back: seek(static_cast<double>(status.positionMicros) - Dock::StepMicros); break;
    case Slot::PlayPause: ReplayInjector::SetPaused(!status.paused); break;
    case Slot::Forward: seek(static_cast<double>(status.positionMicros) + Dock::StepMicros); break;
    case Slot::Keyframe: (void)Keyframes::AddHere(status); break;
    case Slot::Speed: ReplayInjector::SetSpeed(NextSpeed(status.speed)); break;
    case Slot::Teleport:
        if (players.size() == 1) {
            ReplayInjector::TeleportTo(players.front().runtimeId);
        } else if (players.size() > 1) {
            pickerOpen = true;
            pickerIndex = 0;
        }
        break;
    case Slot::Editor: result.openReplayEditor = true; break;
    case Slot::Exit:
        if (ExitArmed(now)) {
            exitArmedAt = {};
            ReplayInjector::Exit();
        } else {
            exitArmedAt = now;
        }
        break;
    }
}

void Notice(std::string text, Clock::time_point now) {
    notice = std::move(text);
    noticeAt = now;
}

// Right click: stop following, or follow the player under the crosshair.
void ViewAlongCrosshair(const ReplayInjector::Status& status, const std::vector<ReplayInjector::Player>& players,
    Clock::time_point now) {
    if (status.spectating) {
        ReplayInjector::Spectate(std::nullopt);
        return;
    }
    const auto view = WorldTelemetry::LatestView();
    if (!view || !view->yawDegrees || !view->pitchDegrees) {
        Notice("Can't tell where you're looking yet", now);
        return;
    }
    const auto target = ReplayInjector::PlayerAlongRay(players, view->eye, *view->yawDegrees, *view->pitchDegrees);
    if (!target) {
        Notice("Right-click a player to see through their eyes", now);
        return;
    }
    ReplayInjector::Spectate(target->runtimeId);
}
}

void DrawReplayHotbar(const ReplayInjector::Status& status, const FrameInfo& frame, DrawResult& result) {
    constexpr int SlotCount = static_cast<int>(Hotbar::Slots.size());
    const auto now = Clock::now();
    const auto players = ReplayInjector::Players();
    const bool shiftDown = frame.keysDown && ((*frame.keysDown)[VK_SHIFT] || (*frame.keysDown)[VK_LSHIFT]);
    const bool shiftPressed = shiftDown && !Hotbar::shiftWasDown;
    Hotbar::shiftWasDown = shiftDown;
    if (Hotbar::pickerOpen && players.size() < 2) Hotbar::pickerOpen = false;
    if (Hotbar::pickerOpen) {
        const int rows = static_cast<int>(players.size()) + 1;  // + Cancel
        Hotbar::pickerIndex = ((Hotbar::pickerIndex - frame.replayHotbarScroll) % rows + rows) % rows;
        const bool onPlayer = Hotbar::pickerIndex < static_cast<int>(players.size());
        const auto& picked = players[static_cast<std::size_t>(std::min(Hotbar::pickerIndex, rows - 2))];
        if (frame.replayHotbarClicks > 0) {
            if (onPlayer) ReplayInjector::TeleportTo(picked.runtimeId);
            Hotbar::pickerOpen = false;
        } else if (frame.replayHotbarRightClicks > 0) {
            if (onPlayer) ReplayInjector::Spectate(picked.runtimeId);
            Hotbar::pickerOpen = false;
        } else if (shiftPressed) {
            Hotbar::pickerOpen = false;
        }
    } else {
        // Scrolling down moves right, as in Minecraft.
        if (frame.replayHotbarScroll != 0) {
            Hotbar::selected = ((Hotbar::selected - frame.replayHotbarScroll) % SlotCount + SlotCount) % SlotCount;
        }
        const auto chosen = Hotbar::Slots[static_cast<std::size_t>(Hotbar::selected)];
        if (chosen != Hotbar::Slot::Exit) Hotbar::exitArmedAt = {};
        if (frame.replayHotbarClicks > 0) Hotbar::Use(chosen, status, players, now, result);
        if (frame.replayHotbarRightClicks > 0) Hotbar::ViewAlongCrosshair(status, players, now);
        // Sneak stops following, as in Java, and stops a camera path.
        if (shiftPressed && status.spectating) ReplayInjector::Spectate(std::nullopt);
        if (shiftPressed && status.cameraPathPlaying) ReplayInjector::PlayCameraPath(false);
    }
    const auto slot = Hotbar::Slots[static_cast<std::size_t>(Hotbar::selected)];
    // Clean shot: while the camera flies its path, the hotbar says how to
    // stop for a moment, then gets out of the picture.
    if (status.cameraPathPlaying && !Hotbar::wasFlying) Hotbar::flyingSince = now;
    Hotbar::wasFlying = status.cameraPathPlaying;
    if (status.cameraPathPlaying && now - Hotbar::flyingSince > Hotbar::CleanShotDelay) return;

    const auto display = ImGui::GetIO().DisplaySize;
    auto* draw = ImGui::GetForegroundDrawList();
    ImFont* font = CompactFont();
    constexpr float SlotSize = 44.0F;
    constexpr float Gap = 4.0F;
    constexpr float Pad = 5.0F;
    const float width = SlotSize * SlotCount + Gap * (SlotCount - 1) + Pad * 2.0F;
    const ImVec2 min((display.x - width) * 0.5F, display.y - SlotSize - Pad * 2.0F - 26.0F);
    const ImVec2 max(min.x + width, min.y + SlotSize + Pad * 2.0F);

    // Following a player or a camera path: say so, top centre.
    if (status.spectating || status.cameraPathPlaying) {
        const std::string viewing = status.spectating ? std::format("Viewing {}", *status.spectating)
            : std::string("Flying the camera path");
        const char* stop = status.spectating ? "Shift or right-click to stop" : "Shift to stop";
        ImFont* titleFace = TitleFont();
        const ImVec2 size = titleFace->CalcTextSizeA(titleFace->FontSize, FLT_MAX, 0.0F, viewing.c_str());
        const ImVec2 stopSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, stop);
        const ImVec2 boxMin((display.x - std::max(size.x, stopSize.x)) * 0.5F - 16.0F, 14.0F);
        const ImVec2 boxMax((display.x + std::max(size.x, stopSize.x)) * 0.5F + 16.0F, 14.0F + size.y + stopSize.y + 18.0F);
        draw->AddRectFilled(boxMin, boxMax, WithAlpha(Canvas, 0.85F), 8.0F);
        draw->AddRect(boxMin, boxMax, WithAlpha(Pink, 0.6F), 8.0F);
        draw->AddText(titleFace, titleFace->FontSize, ImVec2((display.x - size.x) * 0.5F, boxMin.y + 7.0F), Pink, viewing.c_str());
        draw->AddText(font, font->FontSize, ImVec2((display.x - stopSize.x) * 0.5F, boxMin.y + 11.0F + size.y), Muted, stop);
    }

    // Time and progress above the slots.
    const double length = static_cast<double>(std::max<std::uint64_t>(status.lengthMicros, 1));
    const float fraction = std::clamp(static_cast<float>(static_cast<double>(status.positionMicros) / length), 0.0F, 1.0F);
    const float progressY = min.y - 10.0F;
    draw->AddRectFilled(ImVec2(min.x, progressY - 2.0F), ImVec2(max.x, progressY + 2.0F), WithAlpha(Canvas, 0.85F), 2.0F);
    draw->AddRectFilled(ImVec2(min.x, progressY - 2.0F), ImVec2(min.x + width * fraction, progressY + 2.0F), Pink, 2.0F);
    for (const auto& mark : status.marks) {
        const float x = min.x + width * static_cast<float>(static_cast<double>(mark.positionMicros) / length);
        draw->AddCircleFilled(ImVec2(x, progressY), 2.5F, Dock::Mark);
    }
    const std::string time = std::format("{} / {}", Dock::Timecode(status.positionMicros), Dock::Timecode(status.lengthMicros));
    const ImVec2 timeSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, time.c_str());
    draw->AddText(font, font->FontSize, ImVec2(min.x, progressY - 6.0F - timeSize.y), Text, time.c_str());
    if (status.paused || status.speed != 1.0F) {
        const std::string state = status.paused ? std::string("PAUSED") : std::format("{}x", status.speed);
        const ImVec2 stateSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, state.c_str());
        draw->AddText(font, font->FontSize, ImVec2(max.x - stateSize.x, progressY - 6.0F - stateSize.y),
            status.paused ? Amber : Pink, state.c_str());
    }

    // The selected slot's name (or a short notice), centred above, like a
    // held item's name.
    // Keyframe notices (added, or why not) show the same way.
    if (Keyframes::NoticeShowing(now) && Keyframes::noticeAt > Hotbar::noticeAt) Hotbar::Notice(Keyframes::notice, Keyframes::noticeAt);
    const bool showNotice = !Hotbar::notice.empty() && now - Hotbar::noticeAt < Hotbar::NoticeDuration;
    const std::string name = showNotice ? Hotbar::notice : Hotbar::Name(slot, status, players, now);
    const ImU32 nameColor = showNotice || (slot == Hotbar::Slot::Exit && Hotbar::ExitArmed(now)) ? Amber : Text;
    ImFont* nameFont = BodyFont();
    const ImVec2 nameSize = nameFont->CalcTextSizeA(nameFont->FontSize, FLT_MAX, 0.0F, name.c_str());
    const ImVec2 namePosition((display.x - nameSize.x) * 0.5F, progressY - 12.0F - timeSize.y - nameSize.y);
    draw->AddText(nameFont, nameFont->FontSize, ImVec2(namePosition.x + 1.0F, namePosition.y + 1.0F), IM_COL32(0, 0, 0, 160), name.c_str());
    draw->AddText(nameFont, nameFont->FontSize, namePosition, nameColor, name.c_str());

    if (Hotbar::pickerOpen) {
        constexpr int VisibleRows = 8;
        constexpr float RowHeight = 28.0F;
        const int rows = static_cast<int>(players.size()) + 1;
        const int first = std::clamp(Hotbar::pickerIndex - VisibleRows / 2, 0, std::max(0, rows - VisibleRows));
        const int shown = std::min(rows, VisibleRows);
        const char* title = "Teleport to...";
        const char* help = "Scroll: choose   Click: teleport   Right-click: their view   Shift: cancel";
        ImFont* titleFace = BodyFont();
        const float panelWidth = std::max(300.0F, font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, help).x + 28.0F);
        const float panelHeight = 14.0F + titleFace->FontSize + 10.0F + RowHeight * static_cast<float>(shown) + 10.0F + font->FontSize + 12.0F;
        const ImVec2 panelMin((display.x - panelWidth) * 0.5F, namePosition.y - 10.0F - panelHeight);
        const ImVec2 panelMax(panelMin.x + panelWidth, panelMin.y + panelHeight);
        draw->AddRectFilled(panelMin, panelMax, WithAlpha(Canvas, 0.94F), 10.0F);
        draw->AddRect(panelMin, panelMax, WithAlpha(Pink, 0.5F), 10.0F);
        draw->AddText(titleFace, titleFace->FontSize, ImVec2(panelMin.x + 14.0F, panelMin.y + 12.0F), Text, title);
        float rowY = panelMin.y + 14.0F + titleFace->FontSize + 10.0F;
        for (int row = first; row < first + shown; ++row) {
            const bool cancel = row == rows - 1;
            const bool current = row == Hotbar::pickerIndex;
            const ImVec2 rowMin(panelMin.x + 8.0F, rowY);
            const ImVec2 rowMax(panelMax.x - 8.0F, rowY + RowHeight - 3.0F);
            if (current) {
                draw->AddRectFilled(rowMin, rowMax, PinkDeep, 6.0F);
                draw->AddRect(rowMin, rowMax, Pink, 6.0F, 0, 1.5F);
            }
            const float centerY = (rowMin.y + rowMax.y) * 0.5F;
            const ImU32 color = current ? Pink : cancel ? Muted : Secondary;
            Editor::DrawGlyph(draw, cancel ? Editor::Glyph::Close : Editor::Glyph::Person, ImVec2(rowMin.x + 16.0F, centerY), 7.0F, color);
            std::string label = "Cancel";
            if (!cancel) {
                const auto& player = players[static_cast<std::size_t>(row)];
                label = player.recorder ? player.name + "  (recording)" : player.name;
                if (status.spectating && *status.spectating == player.name) label += "  - viewing";
            }
            draw->AddText(font, font->FontSize, ImVec2(rowMin.x + 32.0F, centerY - font->FontSize * 0.5F), current ? Text : color, label.c_str());
            rowY += RowHeight;
        }
        if (first > 0) draw->AddText(font, font->FontSize, ImVec2(panelMax.x - 24.0F, panelMin.y + 12.0F), Muted, "^");
        if (first + shown < rows) draw->AddText(font, font->FontSize, ImVec2(panelMax.x - 24.0F, rowY - 14.0F), Muted, "v");
        draw->AddText(font, font->FontSize, ImVec2(panelMin.x + 14.0F, panelMax.y - font->FontSize - 10.0F), Muted, help);
    }

    // The slots.
    draw->AddRectFilled(min, max, WithAlpha(Canvas, 0.9F), 8.0F);
    draw->AddRect(min, max, Stroke, 8.0F);
    for (int index = 0; index < SlotCount; ++index) {
        const ImVec2 slotMin(min.x + Pad + (SlotSize + Gap) * static_cast<float>(index), min.y + Pad);
        const ImVec2 slotMax(slotMin.x + SlotSize, slotMin.y + SlotSize);
        const ImVec2 center((slotMin.x + slotMax.x) * 0.5F, (slotMin.y + slotMax.y) * 0.5F);
        const bool isSelected = index == Hotbar::selected;
        draw->AddRectFilled(slotMin, slotMax, isSelected ? PinkDeep : Surface, 6.0F);
        if (isSelected) {
            draw->AddRect(ImVec2(slotMin.x - 1.0F, slotMin.y - 1.0F), ImVec2(slotMax.x + 1.0F, slotMax.y + 1.0F), Pink, 7.0F, 0, 2.0F);
        }
        const ImU32 color = isSelected ? Pink : Secondary;
        switch (Hotbar::Slots[static_cast<std::size_t>(index)]) {
        case Hotbar::Slot::Start: Dock::DrawIcon(draw, Dock::Icon::Start, center, 7.0F, color); break;
        case Hotbar::Slot::Back: Dock::DrawIcon(draw, Dock::Icon::Back, center, 7.0F, color); break;
        case Hotbar::Slot::PlayPause:
            Dock::DrawIcon(draw, status.paused ? Dock::Icon::Play : Dock::Icon::Pause, center, 8.0F, color);
            break;
        case Hotbar::Slot::Forward: Dock::DrawIcon(draw, Dock::Icon::Forward, center, 7.0F, color); break;
        case Hotbar::Slot::Keyframe:
            draw->AddQuad(ImVec2(center.x, center.y - 10.0F), ImVec2(center.x + 10.0F, center.y), ImVec2(center.x, center.y + 10.0F),
                ImVec2(center.x - 10.0F, center.y), color, 1.6F);
            Editor::DrawGlyph(draw, Editor::Glyph::Plus, center, 6.0F, color);
            break;
        case Hotbar::Slot::Speed: {
            const std::string speed = std::format("{}x", status.speed);
            const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, speed.c_str());
            draw->AddText(font, font->FontSize, ImVec2(center.x - size.x * 0.5F, center.y - size.y * 0.5F), color, speed.c_str());
            break;
        }
        case Hotbar::Slot::Teleport: Editor::DrawGlyph(draw, Editor::Glyph::Person, center, 9.0F, color); break;
        case Hotbar::Slot::Editor: Editor::DrawGlyph(draw, Editor::Glyph::Keyframes, center, 8.0F, color); break;
        case Hotbar::Slot::Exit:
            Editor::DrawGlyph(draw, Editor::Glyph::Close, center, 9.0F,
                Hotbar::ExitArmed(now) ? Amber : color);
            break;
        }
        // Slot numbers in the corner, like Minecraft's.
        const std::string number = std::to_string(index + 1);
        draw->AddText(font, font->FontSize * 0.75F, ImVec2(slotMin.x + 4.0F, slotMin.y + 2.0F), WithAlpha(Muted, 0.8F), number.c_str());
    }

    const char* hint = "Scroll: choose   Click: use   Right-click a player: their view   K: editor";
    const ImVec2 hintSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, hint);
    draw->AddText(font, font->FontSize, ImVec2((display.x - hintSize.x) * 0.5F, max.y + 5.0F), WithAlpha(Muted, 0.85F), hint);
}

// Top-center status while a replay is being watched.
void DrawReplayPill() {
    const auto status = ReplayInjector::CurrentStatus();
    const auto display = ImGui::GetIO().DisplaySize;
    const std::string text = status.playing
        ? std::format("REPLAY  {} / {}{}", ClockText(status.positionMicros), ClockText(status.lengthMicros),
            status.paused ? "  PAUSED" : status.speed != 1.0F ? std::format("  {}x", status.speed) : std::string())
        : "REPLAY  " + status.phaseText;
    ImFont* font = CompactFont();
    const ImVec2 textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, text.c_str());
    const ImVec2 size(textSize.x + 28.0F, textSize.y + 12.0F);
    const ImVec2 position((display.x - size.x) * 0.5F, 10.0F);
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(position, ImVec2(position.x + size.x, position.y + size.y), WithAlpha(Canvas, 0.85F), size.y * 0.5F);
    draw->AddCircleFilled(ImVec2(position.x + 11.0F, position.y + size.y * 0.5F), 3.5F, status.paused ? Amber : Pink);
    draw->AddText(font, font->FontSize, ImVec2(position.x + 20.0F, position.y + 6.0F), Text, text.c_str());
}

void DrawReplayPanel(ModuleManager& modules, SteadyTime now) {
    const auto controllers = modules.EnabledModulesOf<IReplayControlModule>();
    const bool watching = ReplayInjector::Running();
    if (controllers.empty() && !watching) return;
    if (watching) {
        // While a replay plays, Minecraft's pause screen gets the editor dock.
        const auto status = ReplayInjector::CurrentStatus();
        if (status.playing) {
            DrawReplayDock(status);
            return;
        }
    }

    ImGui::SetNextWindowPos(ImVec2(16.0F, 16.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(292.0F, 0.0F), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 9.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0F, 12.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(17, 20, 23, 242));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(242, 143, 181, 150));
    ImGui::Begin("##replay-pause-controller", nullptr, ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);
    if (titleFont) ImGui::PushFont(titleFont);
    ImGui::TextUnformatted(watching ? "Replay" : "Replay Mod");
    if (titleFont) ImGui::PopFont();
    if (watching) DrawPlaybackControls();
    else DrawRecorderControls(*controllers.front(), now, 124.0F);
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
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
        // Fills first, so outlines drawn on the same shape stay on top.
        for (const auto& quad : primitives.quads) {
            draw->AddQuadFilled(ImVec2(quad.x[0], quad.y[0]), ImVec2(quad.x[1], quad.y[1]), ImVec2(quad.x[2], quad.y[2]),
                ImVec2(quad.x[3], quad.y[3]), FromArgb(quad.argb));
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

ImU32 DebugToneColor(HudTone tone) {
    switch (tone) {
    case HudTone::Accent: return IM_COL32(255, 255, 85, 255);
    case HudTone::Muted: return IM_COL32(180, 180, 180, 255);
    case HudTone::Warning: return IM_COL32(255, 170, 0, 255);
    case HudTone::Good: return IM_COL32(85, 255, 85, 255);
    case HudTone::Normal: break;
    }
    return IM_COL32(255, 255, 255, 255);
}

std::string FitDebugText(ImFont* font, float fontSize, std::string text, float maximumWidth) {
    if (font->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, text.c_str()).x <= maximumWidth) return text;
    constexpr std::string_view suffix = "...";
    while (!text.empty()) {
        unsigned char removed{};
        do {
            removed = static_cast<unsigned char>(text.back());
            text.pop_back();
        } while (!text.empty() && (removed & 0xC0U) == 0x80U);
        const auto candidate = text + std::string(suffix);
        if (font->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, candidate.c_str()).x <= maximumWidth) return candidate;
    }
    return {};
}

void DrawDebugOverlays(ModuleManager& modules, ImVec2 display) {
    auto* draw = ImGui::GetBackgroundDrawList();
    ImFont* font = CompactFont();
    constexpr ImU32 backdrop = IM_COL32(0, 0, 0, 150);
    for (const auto* module : modules.EnabledModulesOf<IDebugOverlayModule>()) {
        DebugOverlayContent content;
        try { content = module->DebugOverlay(); } catch (...) { continue; }
        if (!content.visible) continue;

        const float scale = std::clamp(content.scale, 0.6F, 2.0F);
        const float fontSize = font->FontSize * scale;
        const float padX = std::max(2.0F, 2.0F * scale);
        const float padY = std::max(1.0F, scale);
        const float lineHeight = std::ceil(fontSize + padY * 2.0F);
        const float groupGap = std::max(3.0F, 4.0F * scale);
        const float margin = std::max(3.0F, 4.0F * scale);
        const float maxTextWidth = std::max(1.0F, display.x * 0.47F - padX * 2.0F);

        const auto column = [&](const std::vector<DebugOverlayGroup>& groups, bool rightAligned) {
            float y = margin;
            for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
                if (groupIndex != 0) y += groupGap;
                for (const auto& row : groups[groupIndex].lines) {
                    if (y + lineHeight > display.y - margin) return;
                    const std::string shown = FitDebugText(font, fontSize, row.text, maxTextWidth);
                    if (shown.empty()) continue;
                    const float textWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, shown.c_str()).x;
                    const float x = rightAligned ? display.x - margin - padX - textWidth : margin + padX;
                    draw->AddRectFilled(ImVec2(x - padX, y), ImVec2(x + textWidth + padX, y + lineHeight), backdrop);
                    draw->AddText(font, fontSize, ImVec2(x, y + padY), DebugToneColor(row.tone), shown.c_str());
                    y += lineHeight;
                }
            }
        };
        column(content.left, false);
        column(content.right, true);
    }
}

// Module notices (INoticeModule): the newest one, briefly, near the top.
struct ModuleNotice {
    std::string text;
    Clock::time_point at{};
};
ModuleNotice moduleNotice;

void DrawModuleNotices(ModuleManager& modules, ImVec2 display) {
    for (auto* module : modules.EnabledModulesOf<INoticeModule>()) {
        std::vector<std::string> notices;
        try { notices = module->TakeNotices(); } catch (...) { continue; }
        if (!notices.empty()) moduleNotice = {notices.back(), Clock::now()};
    }
    if (moduleNotice.text.empty()) return;
    constexpr float Duration = 3.0F;
    const float elapsed = std::chrono::duration<float>(Clock::now() - moduleNotice.at).count();
    if (elapsed > Duration) {
        moduleNotice.text.clear();
        return;
    }
    const float alpha = elapsed > Duration - 0.4F ? (Duration - elapsed) / 0.4F : 1.0F;
    auto* draw = ImGui::GetForegroundDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(moduleNotice.text.c_str());
    const ImVec2 size(textSize.x + 28.0F, textSize.y + 14.0F);
    const ImVec2 position(std::round((display.x - size.x) * 0.5F), std::round(display.y * 0.18F));
    draw->AddRectFilled(position, ImVec2(position.x + size.x, position.y + size.y), WithAlpha(Canvas, 0.92F * alpha), 8.0F);
    draw->AddRect(position, ImVec2(position.x + size.x, position.y + size.y), WithAlpha(Pink, alpha), 8.0F);
    draw->AddText(ImVec2(position.x + 14.0F, position.y + 7.0F), WithAlpha(Text, alpha), moduleNotice.text.c_str());
}

// Java-style tab list: a centred panel near the top of the screen with a
// grid of player heads and names.
void DrawPlayerLists(ModuleManager& modules, ImVec2 display) {
    for (const auto* module : modules.EnabledModulesOf<IPlayerListModule>()) {
        PlayerListView view;
        try { view = module->PlayerList(); } catch (...) { continue; }
        if (!view.visible) continue;
        auto* draw = ImGui::GetForegroundDrawList();
        auto* font = ImGui::GetFont();
        const float scale = std::clamp(view.scale, 0.6F, 2.0F);
        const float fontSize = ImGui::GetFontSize() * scale;
        const float rowHeight = std::round(fontSize + 6.0F * scale);
        const float head = view.heads ? std::round(fontSize) : 0.0F;
        const float padding = 10.0F * scale;
        const std::size_t count = view.rows.size();
        const float safeWidth = std::max(40.0F, display.x - 24.0F);
        std::size_t columns = view.columns > 0 ? static_cast<std::size_t>(view.columns)
            : std::clamp<std::size_t>((count + 19) / 20, 1, 4);
        columns = std::max<std::size_t>(1, std::min(columns, std::max<std::size_t>(count, 1)));
        const float fixedCellWidth = (head > 0.0F ? head + 6.0F * scale : 0.0F) + 12.0F * scale;
        const std::size_t maxColumns = std::max<std::size_t>(1,
            static_cast<std::size_t>(std::floor(std::max(1.0F, safeWidth - padding * 2.0F) / (80.0F * scale))));
        columns = std::min(columns, maxColumns);
        const float headerHeight = fontSize * 2.0F + 12.0F * scale;
        const std::size_t maxRows = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(
            std::max(rowHeight, display.y - 24.0F * scale - headerHeight - padding) / rowHeight)));
        const std::size_t shownCount = std::min(count, columns * maxRows);
        const std::size_t rows = std::max<std::size_t>(1, (shownCount + columns - 1) / columns);
        float nameWidth = 60.0F * scale;
        for (const auto& row : view.rows) {
            nameWidth = std::max(nameWidth, font->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, row.name.c_str()).x);
        }
        const float maxNameWidth = std::max(20.0F, (safeWidth - padding * 2.0F) / static_cast<float>(columns) - fixedCellWidth);
        nameWidth = std::min(nameWidth, maxNameWidth);
        const float cellWidth = fixedCellWidth + nameWidth;
        const std::string title = FitDebugText(font, fontSize, view.title.empty() ? std::string("Players") : view.title,
            safeWidth - padding * 2.0F);
        const std::string subtitle = std::format("{} online", count);
        const float titleWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, title.c_str()).x;
        const float width = std::min(safeWidth,
            std::max(cellWidth * static_cast<float>(columns), titleWidth + 40.0F * scale) + padding * 2.0F);
        const float height = headerHeight + rowHeight * static_cast<float>(rows) + padding;
        const ImVec2 origin(std::round((display.x - width) * 0.5F), std::round(24.0F * scale));
        draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), WithAlpha(Canvas, 0.92F), 8.0F * scale);
        draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height), Stroke, 8.0F * scale);
        draw->AddText(font, fontSize, ImVec2(origin.x + (width - titleWidth) * 0.5F, origin.y + padding * 0.6F), Text, title.c_str());
        const float subtitleWidth = font->CalcTextSizeA(fontSize * 0.85F, FLT_MAX, 0.0F, subtitle.c_str()).x;
        draw->AddText(font, fontSize * 0.85F, ImVec2(origin.x + (width - subtitleWidth) * 0.5F, origin.y + padding * 0.6F + fontSize + 2.0F),
            Pink, subtitle.c_str());
        const float gridLeft = origin.x + (width - cellWidth * static_cast<float>(columns)) * 0.5F;
        for (std::size_t index = 0; index < shownCount; ++index) {
            const auto& row = view.rows[index];
            // Column-major, like Java's list.
            const std::size_t column = index / rows;
            const std::size_t line = index % rows;
            const ImVec2 cell(gridLeft + cellWidth * static_cast<float>(column),
                origin.y + headerHeight + rowHeight * static_cast<float>(line));
            draw->AddRectFilled(ImVec2(cell.x + 2.0F, cell.y + 1.0F), ImVec2(cell.x + cellWidth - 2.0F, cell.y + rowHeight - 1.0F),
                line % 2 == 0 ? WithAlpha(Surface, 0.85F) : WithAlpha(SurfaceRaised, 0.85F), 3.0F * scale);
            float textX = cell.x + 6.0F * scale;
            if (head > 0.0F) {
                const float pixel = head / 8.0F;
                const ImVec2 headOrigin(textX, cell.y + (rowHeight - head) * 0.5F);
                if (row.hasFace) {
                    for (std::size_t p = 0; p < row.face.size(); ++p) {
                        const float px = headOrigin.x + static_cast<float>(p % 8) * pixel;
                        const float py = headOrigin.y + static_cast<float>(p / 8) * pixel;
                        draw->AddRectFilled(ImVec2(px, py), ImVec2(px + pixel, py + pixel), FromArgb(row.face[p]));
                    }
                } else {
                    draw->AddRectFilled(headOrigin, ImVec2(headOrigin.x + head, headOrigin.y + head), Stroke, 2.0F);
                }
                textX += head + 6.0F * scale;
            }
            const auto shownName = FitDebugText(font, fontSize, row.name, nameWidth);
            draw->AddText(font, fontSize, ImVec2(textX, cell.y + (rowHeight - fontSize) * 0.5F), Text, shownName.c_str());
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

void DrawHomeHub(ModuleManager& modules, float deltaSeconds, bool& editMode) {
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
        "", scale, "Edit GUI - arrange your HUD")) editMode = true;
    if (HubTile("##modules-hub", ImVec2(startX + sideSize.x + gap, dockY), centerSize,
        HubIcon::Modules, "MODS", scale, "Modules - browse fair-play tools")) section = Section::Modules;
    if (HubTile("##settings-hub", ImVec2(startX + sideSize.x + gap + centerSize.x + gap, dockY),
        sideSize, HubIcon::Settings, "", scale, "Settings - controls and status")) section = Section::Settings;
    const ImVec2 profileSize(220.0F * scale, 58.0F * scale);
    if (HubTile("##profiles-hub", ImVec2((display.x - profileSize.x) * 0.5F, dockY + sideSize.y + gap),
        profileSize, HubIcon::Modules, "PROFILES", scale, "Profiles - switch complete setups")) section = Section::Profiles;
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

// A 16x16 paint grid: left-drag paints, right-drag erases. Returns true and
// updates `grid` only when a pixel actually changed.
bool PixelGridEditor(const char* id, PixelGrid& grid, const PixelGrid& fallback, float width) {
    const float cell = std::clamp(std::floor(width / static_cast<float>(PixelGridSize)), 8.0F, 16.0F);
    const float edge = cell * static_cast<float>(PixelGridSize);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(edge, edge), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool painting = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool erasing = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right);

    bool changed = false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto column = static_cast<int>(std::floor((mouse.x - origin.x) / cell));
    const auto row = static_cast<int>(std::floor((mouse.y - origin.y) / cell));
    const bool inside = column >= 0 && row >= 0 && column < static_cast<int>(PixelGridSize) && row < static_cast<int>(PixelGridSize);
    if (inside && (painting || erasing)) {
        const auto x = static_cast<std::size_t>(column);
        const auto y = static_cast<std::size_t>(row);
        if (PixelAt(grid, x, y) != painting) {
            SetPixel(grid, x, y, painting);
            changed = true;
        }
    }

    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + edge, origin.y + edge), Surface, 4.0F);
    for (std::size_t y = 0; y < PixelGridSize; ++y) {
        for (std::size_t x = 0; x < PixelGridSize; ++x) {
            if (!PixelAt(grid, x, y)) continue;
            const ImVec2 minimum(origin.x + static_cast<float>(x) * cell, origin.y + static_cast<float>(y) * cell);
            draw->AddRectFilled(minimum, ImVec2(minimum.x + cell, minimum.y + cell), Text);
        }
    }
    for (std::size_t line = 1; line < PixelGridSize; ++line) {
        const float offset = static_cast<float>(line) * cell;
        const ImU32 color = line == PixelGridSize / 2 ? WithAlpha(Pink, 0.6F) : WithAlpha(Stroke, 0.55F);
        draw->AddLine(ImVec2(origin.x + offset, origin.y), ImVec2(origin.x + offset, origin.y + edge), color);
        draw->AddLine(ImVec2(origin.x, origin.y + offset), ImVec2(origin.x + edge, origin.y + offset), color);
    }
    if (hovered && inside) {
        const ImVec2 minimum(origin.x + static_cast<float>(column) * cell, origin.y + static_cast<float>(row) * cell);
        draw->AddRect(minimum, ImVec2(minimum.x + cell, minimum.y + cell), Pink, 0.0F, 0, 1.5F);
    }
    draw->AddRect(origin, ImVec2(origin.x + edge, origin.y + edge), Stroke, 4.0F);

    const auto edit = [&](const char* label, const PixelGrid& next) {
        if (ImGui::SmallButton(label) && next != grid) {
            grid = next;
            changed = true;
        }
    };
    edit("Clear", PixelGrid{});
    ImGui::SameLine();
    edit("Mirror left to right", MirrorPixelsHorizontally(grid));
    ImGui::SameLine();
    edit("Mirror top to bottom", MirrorPixelsVertically(grid));
    ImGui::SameLine();
    edit("Default", fallback);
    return changed;
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
    case SettingType::Pixels: {
        auto grid = reader.Pixels(descriptor.key);
        FormLabel(form, descriptor.label, help);
        ImGui::NewLine();
        FormIndent(form, rowStartX);
        ImGui::BeginGroup();
        if (PixelGridEditor(id.c_str(), grid, ParsePixelGrid(descriptor.defaultValue).value_or(PixelGrid{}), form.controlWidth)) {
            value = FormatPixelGrid(grid);
            changed = true;
        }
        ImGui::EndGroup();
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
            if (!IsSettingVisible(descriptor, reader)) continue;
            // A change can show or hide later rows; they appear next frame.
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

std::filesystem::path ReplayDirectory() {
    wchar_t localAppData[32768]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
        static_cast<DWORD>(std::size(localAppData)));
    if (length > 0 && length < std::size(localAppData)) {
        return std::filesystem::path(localAppData) / L"Oinkster" / L"Client" / L"replays";
    }
    return std::filesystem::temp_directory_path() / L"Oinkster" / L"replays";
}

// Replays theater: intro/loading -> library of saved recordings -> viewer.
// Everything runs on the render thread; files are summarized one per frame
// behind the intro animation, so there is no worker to drain on unload.
enum class TheaterPhase { Intro, Library, ViewerLoading, Viewer };

struct TheaterEntry {
    std::filesystem::file_time_type modified{};
    Replay::ReplaySummary summary;
};

struct Theater {
    TheaterPhase phase{TheaterPhase::Intro};
    std::uint64_t lastFrame{};
    float phaseSeconds{};
    float introSeconds{};
    bool introPlayed{};
    std::vector<std::filesystem::path> pending;
    std::size_t total{};
    std::unordered_map<std::wstring, TheaterEntry> cache;
    std::vector<std::wstring> order;
    // The replay a Delete or Rename dialog is about (its cache key), and the
    // name being typed. Esc closes an open dialog before anything else.
    enum class Dialog { None, Delete, Rename } dialog{Dialog::None};
    std::wstring dialogKey;
    std::array<char, 96> renameBuffer{};
    std::string renameError;
    std::filesystem::path openPath;
    std::optional<Replay::ReplayTimeline> timeline;
    std::vector<Replay::PathPoint> drawPath;
    double playheadMicros{};
    bool playing{};
    float speed{1.0F};
    // Opened from the main-menu button: no ClickGUI chrome, no recorder.
    bool standalone{};
    bool closeRequested{};
    std::string watchError;
    std::filesystem::path watchPath;
    // Watch was pressed and the replay is loading or waiting for a world.
    bool watchArmed{};
};
Theater theater;

constexpr std::size_t MaxListedReplays = 200;

bool IsReplayFile(const std::filesystem::path& path) {
    const auto extension = path.extension().wstring();
    return extension == L".oinkreplay" || extension == L".partial";
}

void SortTheaterOrder() {
    theater.order.clear();
    for (const auto& [key, entry] : theater.cache) theater.order.push_back(key);
    std::ranges::sort(theater.order, [](const std::wstring& left, const std::wstring& right) {
        const auto& a = theater.cache.at(left);
        const auto& b = theater.cache.at(right);
        if (a.summary.startedUnixMillis != b.summary.startedUnixMillis) {
            return a.summary.startedUnixMillis > b.summary.startedUnixMillis;
        }
        return a.modified > b.modified;
    });
}

// Rescans the folder, keeping cached summaries whose file is unchanged.
void EnterTheater() {
    theater.pending.clear();
    theater.dialog = Theater::Dialog::None;
    theater.dialogKey.clear();
    std::unordered_map<std::wstring, TheaterEntry> kept;
    std::error_code error;
    const auto directory = ReplayDirectory();
    std::size_t listed = 0;
    if (std::filesystem::exists(directory, error)) {
        for (std::filesystem::directory_iterator iterator(directory, error), end;
             !error && iterator != end && listed < MaxListedReplays; iterator.increment(error)) {
            if (!iterator->is_regular_file(error) || !IsReplayFile(iterator->path())) continue;
            ++listed;
            const auto key = iterator->path().wstring();
            const auto modified = iterator->last_write_time(error);
            const auto bytes = iterator->file_size(error);
            const auto cached = theater.cache.find(key);
            if (cached != theater.cache.end() && cached->second.modified == modified
                && cached->second.summary.bytes == bytes) {
                kept.emplace(key, std::move(cached->second));
            } else {
                theater.pending.push_back(iterator->path());
            }
        }
    }
    theater.cache = std::move(kept);
    theater.total = theater.cache.size() + theater.pending.size();
    theater.phase = TheaterPhase::Intro;
    theater.phaseSeconds = 0.0F;
    // The full intro plays once per session; later visits get a short fade.
    theater.introSeconds = theater.introPlayed ? 0.3F : 0.9F;
    theater.introPlayed = true;
    SortTheaterOrder();
}

void LoadPendingSummary() {
    if (theater.pending.empty()) return;
    const auto path = theater.pending.back();
    theater.pending.pop_back();
    std::error_code error;
    TheaterEntry entry{std::filesystem::last_write_time(path, error), Replay::Summarize(path)};
    theater.cache[path.wstring()] = std::move(entry);
    if (theater.pending.empty()) SortTheaterOrder();
}

std::string FormatDuration(std::uint64_t micros) {
    const auto seconds = micros / 1'000'000;
    if (seconds >= 3600) return std::format("{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60);
    return std::format("{:02}:{:02}", seconds / 60, seconds % 60);
}

std::string FormatStarted(std::uint64_t unixMillis) {
    if (unixMillis == 0) return "Unknown date";
    const auto seconds = static_cast<std::time_t>(unixMillis / 1000);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) return "Unknown date";
    constexpr std::array<const char*, 12> Months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return std::format("{} {}, {:02}:{:02}", Months.at(static_cast<std::size_t>(local.tm_mon) % 12),
        local.tm_mday, local.tm_hour, local.tm_min);
}

const char* DimensionName(std::optional<int> dimension) {
    if (!dimension) return "Unknown";
    switch (*dimension) {
    case 0: return "Overworld";
    case 1: return "Nether";
    case 2: return "The End";
    default: return "Other";
    }
}

// Maps world X/Z into a screen rectangle with one uniform scale (X right,
// Z down, i.e. north up) so the path keeps its real shape.
struct MapProjection {
    ImVec2 origin;
    ImVec2 size;
    double minX{};
    double minZ{};
    double scale{1.0};
    ImVec2 offset;

    MapProjection(ImVec2 position, ImVec2 extent, double lowX, double highX, double lowZ, double highZ, float padding)
        : origin(position), size(extent), minX(lowX), minZ(lowZ) {
        const double spanX = std::max(highX - lowX, 8.0);
        const double spanZ = std::max(highZ - lowZ, 8.0);
        const double usableX = std::max(1.0, static_cast<double>(extent.x - padding * 2.0F));
        const double usableY = std::max(1.0, static_cast<double>(extent.y - padding * 2.0F));
        scale = std::min(usableX / spanX, usableY / spanZ);
        offset = ImVec2(static_cast<float>((extent.x - (highX - lowX) * scale) * 0.5),
            static_cast<float>((extent.y - (highZ - lowZ) * scale) * 0.5));
    }

    [[nodiscard]] ImVec2 operator()(double x, double z) const {
        return ImVec2(origin.x + offset.x + static_cast<float>((x - minX) * scale),
            origin.y + offset.y + static_cast<float>((z - minZ) * scale));
    }
};

void DrawPathThumbnail(ImDrawList* draw, const Replay::ReplaySummary& summary, ImVec2 position, ImVec2 size) {
    draw->AddRectFilled(position, ImVec2(position.x + size.x, position.y + size.y), Rail, 8.0F);
    if (summary.thumbnail.size() < 2) {
        const char* label = summary.thumbnail.empty() ? "no path" : "stood still";
        ImFont* font = CompactFont();
        const ImVec2 textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, label);
        draw->AddText(font, font->FontSize, ImVec2(position.x + (size.x - textSize.x) * 0.5F,
            position.y + (size.y - textSize.y) * 0.5F), Muted, label);
        return;
    }
    const MapProjection project(position, size, summary.minX, summary.maxX, summary.minZ, summary.maxZ, 10.0F);
    std::vector<ImVec2> points;
    points.reserve(summary.thumbnail.size());
    for (const auto& point : summary.thumbnail) points.push_back(project(point.x, point.z));
    draw->AddPolyline(points.data(), static_cast<int>(points.size()), WithAlpha(Pink, 0.85F), ImDrawFlags_None, 1.6F);
    draw->AddCircleFilled(points.front(), 3.0F, Green);
    draw->AddCircleFilled(points.back(), 3.0F, Pink);
}

// Recordings are saved as replay-<unix ms>; anything else was renamed.
bool HasDefaultName(const std::filesystem::path& path) {
    const auto stem = path.stem().wstring();
    constexpr std::wstring_view Prefix = L"replay-";
    return stem.size() > Prefix.size() && stem.starts_with(Prefix)
        && std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(Prefix.size()), stem.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

// A replay's card title: its name, or when it was recorded.
std::string ReplayTitle(const Replay::ReplaySummary& summary) {
    if (HasDefaultName(summary.path)) return FormatStarted(summary.startedUnixMillis);
    return Narrow(summary.path.stem().wstring());
}

std::wstring Widen(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// A typed name as a file name: no characters Windows forbids, no leading or
// trailing spaces or dots. Empty when nothing usable is left.
std::wstring ReplayFileStem(std::string_view typed) {
    std::wstring name;
    for (const wchar_t c : Widen(typed)) {
        if (c < 0x20 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) continue;
        name.push_back(c);
    }
    const auto first = name.find_first_not_of(L" .");
    if (first == std::wstring::npos) return {};
    name = name.substr(first, name.find_last_not_of(L" .") - first + 1);
    if (name.size() > 64) name.resize(64);
    return name;
}

bool DangerButton(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ToVec4(IM_COL32(206, 82, 96, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToVec4(IM_COL32(226, 104, 116, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToVec4(IM_COL32(184, 66, 80, 255)));
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(IM_COL32(255, 244, 246, 255)));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

// A small confirmation card in the overlay's own style: centered, no title
// bar, a rounded panel over a dimmed library.
bool BeginTheaterDialog(const char* id, float width) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5F, display.y * 0.5F), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0F), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ToVec4(Surface));
    ImGui::PushStyleColor(ImGuiCol_Border, ToVec4(Stroke));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ToVec4(IM_COL32(10, 6, 8, 150)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0F, 18.0F));
    const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    return open;
}

void DialogHeading(const char* title) {
    ImGui::PushFont(BodyFont());
    ImGui::TextColored(ToVec4(Text), "%s", title);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0F, 2.0F));
}

// Right-aligns a row of two buttons of `width` each.
void DialogButtonRow(float width) {
    ImGui::Dummy(ImVec2(0.0F, 8.0F));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width * 2.0F - ImGui::GetStyle().ItemSpacing.x);
}

// The Delete and Rename dialogs for theater.dialogKey. True when the files
// changed and the library must be rescanned.
bool DrawReplayDialogs() {
    constexpr const char* DeleteId = "##delete-replay";
    constexpr const char* RenameId = "##rename-replay";
    const auto found = theater.cache.find(theater.dialogKey);
    if (theater.dialog != Theater::Dialog::None && found == theater.cache.end()) theater.dialog = Theater::Dialog::None;
    if (theater.dialog == Theater::Dialog::Delete && !ImGui::IsPopupOpen(DeleteId)) ImGui::OpenPopup(DeleteId);
    if (theater.dialog == Theater::Dialog::Rename && !ImGui::IsPopupOpen(RenameId)) ImGui::OpenPopup(RenameId);
    bool changed = false;
    constexpr float ButtonWidth = 96.0F;

    if (BeginTheaterDialog(DeleteId, 340.0F)) {
        if (theater.dialog != Theater::Dialog::Delete) {
            ImGui::CloseCurrentPopup();
        } else {
            const auto& summary = found->second.summary;
            DialogHeading("Delete replay?");
            ImGui::PushTextWrapPos();
            ImGui::PushFont(CompactFont());
            ImGui::TextColored(ToVec4(Secondary), "%s", ReplayTitle(summary).c_str());
            ImGui::TextColored(ToVec4(Muted), "This recording will be permanently removed. This can't be undone.");
            ImGui::PopFont();
            ImGui::PopTextWrapPos();
            DialogButtonRow(ButtonWidth);
            if (ImGui::Button("Cancel", ImVec2(ButtonWidth, 30.0F))) theater.dialog = Theater::Dialog::None;
            ImGui::SameLine();
            if (DangerButton("Delete", ImVec2(ButtonWidth, 30.0F))) {
                std::error_code ignored;
                std::filesystem::remove(summary.path, ignored);
                std::filesystem::remove(ReplayInjector::CameraPathFile(summary.path), ignored);
                theater.dialog = Theater::Dialog::None;
                changed = true;
            }
            if (theater.dialog == Theater::Dialog::None) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (BeginTheaterDialog(RenameId, 360.0F)) {
        if (theater.dialog != Theater::Dialog::Rename) {
            ImGui::CloseCurrentPopup();
        } else {
            const auto& summary = found->second.summary;
            DialogHeading("Rename replay");
            CaptionText(Muted, FormatStarted(summary.startedUnixMillis).c_str());
            ImGui::Dummy(ImVec2(0.0F, 4.0F));
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool entered = ImGui::InputTextWithHint("##replay-name", "Name this replay", theater.renameBuffer.data(),
                theater.renameBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            if (!theater.renameError.empty()) {
                ImGui::PushTextWrapPos();
                CaptionText(Amber, theater.renameError.c_str());
                ImGui::PopTextWrapPos();
            }
            DialogButtonRow(ButtonWidth);
            if (ImGui::Button("Cancel", ImVec2(ButtonWidth, 30.0F))) theater.dialog = Theater::Dialog::None;
            ImGui::SameLine();
            if (PrimaryButton("Save", ImVec2(ButtonWidth, 30.0F)) || entered) {
                const auto stem = ReplayFileStem(theater.renameBuffer.data());
                const auto target = summary.path.parent_path() / (stem + summary.path.extension().wstring());
                std::error_code error;
                if (stem.empty()) {
                    theater.renameError = "Type a name first.";
                } else if (target == summary.path) {
                    theater.dialog = Theater::Dialog::None;
                } else if (std::filesystem::exists(target, error)) {
                    theater.renameError = "Another replay already has that name.";
                } else {
                    std::filesystem::rename(summary.path, target, error);
                    // Its camera keyframes go with it.
                    if (std::error_code ignored; !error && std::filesystem::exists(ReplayInjector::CameraPathFile(summary.path), ignored)) {
                        std::filesystem::rename(ReplayInjector::CameraPathFile(summary.path), ReplayInjector::CameraPathFile(target), ignored);
                    }
                    if (error) {
                        theater.renameError = "The replay could not be renamed.";
                    } else {
                        theater.dialog = Theater::Dialog::None;
                        changed = true;
                    }
                }
            }
            if (theater.dialog == Theater::Dialog::None) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return changed;
}

void DrawTheaterIntro(float deltaSeconds) {
    theater.phaseSeconds += deltaSeconds;
    // One file per frame keeps a long recording from stalling several frames.
    LoadPendingSummary();
    const bool loaded = theater.pending.empty();
    const bool skip = loaded && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Space, false));
    if (loaded && (theater.phaseSeconds >= theater.introSeconds || skip)) {
        theater.phase = TheaterPhase::Library;
        theater.phaseSeconds = 0.0F;
        return;
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area = ImGui::GetContentRegionAvail();
    auto* draw = ImGui::GetWindowDrawList();
    const float t = theater.phaseSeconds;
    const float appear = std::clamp(t / 0.35F, 0.0F, 1.0F);
    const ImVec2 center(origin.x + area.x * 0.5F, origin.y + area.y * 0.42F);

    // Two film reels spinning either side of the snout.
    for (int side = -1; side <= 1; side += 2) {
        const ImVec2 reel(center.x + static_cast<float>(side) * 74.0F, center.y);
        draw->AddCircle(reel, 34.0F, WithAlpha(Pink, appear), 48, 3.0F);
        draw->AddCircleFilled(reel, 7.0F, WithAlpha(Pink, appear));
        for (int hole = 0; hole < 5; ++hole) {
            const float angle = t * 3.2F * static_cast<float>(side) + static_cast<float>(hole) * 1.2566F;
            draw->AddCircleFilled(ImVec2(reel.x + std::cos(angle) * 20.0F, reel.y + std::sin(angle) * 20.0F),
                6.0F, WithAlpha(PinkDeep, appear));
            draw->AddCircle(ImVec2(reel.x + std::cos(angle) * 20.0F, reel.y + std::sin(angle) * 20.0F),
                6.0F, WithAlpha(Pink, appear * 0.8F), 16, 1.5F);
        }
    }
    // Film strip running between the reels.
    const float stripY = center.y + 46.0F;
    draw->AddRectFilled(ImVec2(center.x - 120.0F, stripY), ImVec2(center.x + 120.0F, stripY + 18.0F),
        WithAlpha(Surface, appear), 3.0F);
    const float scroll = std::fmod(t * 60.0F, 20.0F);
    draw->PushClipRect(ImVec2(center.x - 120.0F, stripY), ImVec2(center.x + 120.0F, stripY + 18.0F), true);
    for (float x = center.x - 140.0F + scroll; x < center.x + 140.0F; x += 20.0F) {
        draw->AddRectFilled(ImVec2(x, stripY + 3.0F), ImVec2(x + 8.0F, stripY + 7.0F), WithAlpha(Muted, appear), 1.0F);
        draw->AddRectFilled(ImVec2(x, stripY + 11.0F), ImVec2(x + 8.0F, stripY + 15.0F), WithAlpha(Muted, appear), 1.0F);
    }
    draw->PopClipRect();
    const float bob = std::sin(t * 4.0F) * 2.0F;
    DrawSnout(draw, ImVec2(center.x, center.y + bob), 1.15F, appear);

    ImFont* title = TitleFont();
    const char* heading = "OINKSTER REPLAYS";
    const ImVec2 headingSize = title->CalcTextSizeA(title->FontSize, FLT_MAX, 0.0F, heading);
    draw->AddText(title, title->FontSize, ImVec2(center.x - headingSize.x * 0.5F, stripY + 34.0F),
        WithAlpha(Text, appear), heading);

    // Progress follows real loading; the timer only guarantees a minimum.
    const float loadProgress = theater.total == 0 ? 1.0F
        : static_cast<float>(theater.total - theater.pending.size()) / static_cast<float>(theater.total);
    const float progress = std::min(loadProgress, std::clamp(t / std::max(theater.introSeconds, 0.01F), 0.0F, 1.0F));
    const float barY = stripY + 44.0F + headingSize.y;
    const ImVec2 barMin(center.x - 140.0F, barY);
    draw->AddRectFilled(barMin, ImVec2(center.x + 140.0F, barY + 5.0F), WithAlpha(Stroke, appear), 2.5F);
    draw->AddRectFilled(barMin, ImVec2(barMin.x + 280.0F * progress, barY + 5.0F), WithAlpha(Pink, appear), 2.5F);
    const std::string status = theater.total == 0 ? std::string("Loading replays...")
        : std::format("Loading replays... {} / {}", theater.total - theater.pending.size(), theater.total);
    ImFont* compact = CompactFont();
    const ImVec2 statusSize = compact->CalcTextSizeA(compact->FontSize, FLT_MAX, 0.0F, status.c_str());
    draw->AddText(compact, compact->FontSize, ImVec2(center.x - statusSize.x * 0.5F, barY + 12.0F),
        WithAlpha(Muted, appear), status.c_str());
}

void DrawRecorderCard(ModuleManager& modules, SteadyTime now) {
    const auto controllers = modules.EnabledModulesOf<IReplayControlModule>();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ToVec4(Surface));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0F, 10.0F));
    ImGui::BeginChild("##recorder-card", ImVec2(0.0F, 0.0F),
        ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
    CaptionText(Pink, "RECORDER");
    if (controllers.empty()) {
        CaptionText(Muted, "Enable Replay Mod in MODS to record your sessions.");
    } else {
        auto* replay = controllers.front();
        const float width = std::min(124.0F, (ImGui::GetContentRegionAvail().x - 8.0F) * 0.5F);
        DrawRecorderControls(*replay, now, width);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void OpenReplay(const std::filesystem::path& path) {
    theater.openPath = path;
    theater.timeline.reset();
    theater.drawPath.clear();
    theater.playheadMicros = 0.0;
    theater.playing = false;
    theater.speed = 1.0F;
    theater.phase = TheaterPhase::ViewerLoading;
    theater.phaseSeconds = 0.0F;
}

// Loads the replay and waits for the viewer to open one of their own local
// worlds; the recording then replaces that world's packets in-game
// (ReplayInjector), with the viewer in spectator mode.
void WatchReplay(const std::filesystem::path& path) {
    Keyframes::Reset();
    std::string error;
    theater.watchPath = path;
    if (!ReplayInjector::Watch(path, error)) {
        theater.watchError = error;
        theater.watchArmed = false;
        return;
    }
    theater.watchError.clear();
    theater.watchArmed = true;
}

void UpdateWatchLaunch() {
    if (!theater.watchArmed) return;
    const auto status = ReplayInjector::CurrentStatus();
    if (status.phase == ReplayInjector::Phase::Failed) {
        theater.watchArmed = false;
        theater.watchError = status.error.empty() ? "The replay could not be loaded." : status.error;
        return;
    }
    if (!status.running) {
        theater.watchArmed = false;
        return;
    }
    // Loaded: start the private replay world (or, if this build could not be
    // matched, get the theater out of the way so a world can be opened).
    if (status.phase == ReplayInjector::Phase::WaitingForWorld && theater.standalone) {
        theater.watchArmed = false;
        if (ReplayWorld::Available()) ReplayWorld::RequestStart();
        theater.closeRequested = true;
    }
}

void DrawTheaterLibrary(ModuleManager& modules, SteadyTime now) {
    UpdateWatchLaunch();
    PageHeader("Replays", theater.order.empty() ? "Your saved recordings"
        : std::format("{} saved recording{}", theater.order.size(), theater.order.size() == 1 ? "" : "s").c_str());
    const float headerRight = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(headerRight - (theater.standalone ? 334.0F : 254.0F));
    if (ImGui::Button("Videos", ImVec2(76.0F, 0.0F))) {
        std::error_code ignored;
        const auto directory = Render::VideoDirectory();
        std::filesystem::create_directories(directory, ignored);
        ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open the folder rendered videos are saved in");
    ImGui::SameLine();
    if (ImGui::Button("Open folder", ImVec2(96.0F, 0.0F))) {
        std::error_code ignored;
        const auto directory = ReplayDirectory();
        std::filesystem::create_directories(directory, ignored);
        ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(68.0F, 0.0F))) EnterTheater();
    if (theater.standalone) {
        ImGui::SameLine();
        if (PrimaryButton("Done", ImVec2(72.0F, 0.0F))) theater.closeRequested = true;
    }
    ImGui::Dummy(ImVec2(0.0F, 4.0F));
    if (ReplayInjector::Running()) {
        const auto status = ReplayInjector::CurrentStatus();
        ImGui::TextColored(ToVec4(Pink), "%s", status.phaseText.c_str());
        if (status.phase == ReplayInjector::Phase::WaitingForWorld) {
            const auto worldError = ReplayWorld::LastError();
            if (!worldError.empty()) {
                ImGui::PushTextWrapPos();
                ImGui::TextColored(ToVec4(Amber), "%s", worldError.c_str());
                ImGui::PopTextWrapPos();
            }
            if (ReplayWorld::Available() && ImGui::Button("Start replay world", ImVec2(150.0F, 0.0F))) {
                ReplayWorld::RequestStart();
                theater.closeRequested = true;
            }
            CaptionText(Muted, ReplayWorld::Available()
                ? "Or open any singleplayer world; the replay takes it over."
                : "Open any singleplayer world; the replay takes it over. Use a throwaway world.");
        }
        if (ImGui::Button("Cancel replay", ImVec2(124.0F, 0.0F))) {
            ReplayInjector::Stop();
            theater.watchArmed = false;
        }
    }
    if (!theater.watchError.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ToVec4(Amber), "%s", theater.watchError.c_str());
        ImGui::PopTextWrapPos();
        if (!theater.watchPath.empty() && ImGui::Button("Retry Watch", ImVec2(112.0F, 0.0F))) {
            WatchReplay(theater.watchPath);
        }
    }
    if (!theater.standalone) CaptionText(Muted, "Watch replays from the Replays button on Minecraft's title screen.");
    if (!theater.standalone) {
        DrawRecorderCard(modules, now);
        ImGui::Dummy(ImVec2(0.0F, 8.0F));
    }

    if (theater.order.empty()) {
        ImGui::TextColored(ToVec4(Secondary), "No replays yet.");
        CaptionText(Muted, "Enable Replay Mod, join a world, then press Start in the Esc menu or the K menu.");
        return;
    }

    // A recorder that is still writing owns its .partial file.
    bool recording = false;
    for (auto* replay : modules.EnabledModulesOf<IReplayControlModule>()) {
        const auto state = replay->ReplaySnapshot(now).state;
        recording |= state == ReplayControlState::Recording || state == ReplayControlState::Paused;
    }

    ImGui::BeginChild("##replay-grid", ImVec2(0.0F, 0.0F), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    const float available = ImGui::GetContentRegionAvail().x;
    constexpr float Gap = 12.0F;
    const int columns = available >= 560.0F ? 2 : 1;
    const float cardWidth = (available - Gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    constexpr float CardHeight = 112.0F;
    constexpr float ActionHeight = 26.0F;
    auto* draw = ImGui::GetWindowDrawList();
    std::optional<std::filesystem::path> open;
    std::optional<std::filesystem::path> watch;
    int column = 0;
    for (const auto& key : theater.order) {
        const auto& summary = theater.cache.at(key).summary;
        const bool partial = summary.path.extension() == L".partial";
        ImGui::PushID(static_cast<int>(std::hash<std::wstring>{}(key)));
        if (column > 0) ImGui::SameLine(0.0F, Gap);
        ImGui::BeginGroup();
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##card", ImVec2(cardWidth, CardHeight));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 maximum(position.x + cardWidth, position.y + CardHeight + ActionHeight);
        draw->AddRectFilled(position, maximum, hovered ? SurfaceRaised : Surface, 8.0F);
        draw->AddRect(position, maximum, hovered ? Pink : Stroke, 8.0F, 0, hovered ? 1.6F : 1.0F);
        const ImVec2 thumbSize(CardHeight - 20.0F, CardHeight - 20.0F);
        DrawPathThumbnail(draw, summary, ImVec2(position.x + 10.0F, position.y + 10.0F), thumbSize);

        const float textX = position.x + 20.0F + thumbSize.x;
        const float textWidth = cardWidth - (textX - position.x) - 10.0F;
        draw->AddText(BodyFont(), BodyFont()->FontSize, ImVec2(textX, position.y + 12.0F), Text,
            Ellipsize(BodyFont(), ReplayTitle(summary), textWidth).c_str());
        // A named replay keeps its date on the line below.
        const std::string facts = std::format("{}{}{}{} mark{}",
            HasDefaultName(summary.path) ? std::string() : FormatStarted(summary.startedUnixMillis) + Dot,
            FormatDuration(summary.durationMicros), Dot, summary.markCount, summary.markCount == 1 ? "" : "s");
        draw->AddText(CompactFont(), CompactFont()->FontSize, ImVec2(textX, position.y + 40.0F), Secondary,
            Ellipsize(CompactFont(), facts, textWidth).c_str());
        const std::string size = std::format("{:.1f} MB{}{} packets", static_cast<double>(summary.bytes) / (1024.0 * 1024.0), Dot,
            summary.packetCount);
        draw->AddText(CompactFont(), CompactFont()->FontSize, ImVec2(textX, position.y + 58.0F), Muted,
            Ellipsize(CompactFont(), size, textWidth).c_str());
        const char* badge = !summary.error.empty() ? "DAMAGED" : !summary.hasStartGame ? "NO WORLD DATA"
            : partial ? "RECOVERABLE" : "COMPLETE";
        const ImU32 badgeColor = summary.error.empty() && summary.hasStartGame && !partial ? Green : Amber;
        draw->AddText(CompactFont(), CompactFont()->FontSize, ImVec2(textX, position.y + 82.0F), badgeColor, badge);
        if (hovered) ImGui::SetTooltip("%s", Narrow(summary.path.filename().wstring()).c_str());

        const bool playable = summary.hasStartGame && summary.error.empty();
        ImGui::SetCursorScreenPos(ImVec2(textX, position.y + CardHeight + 2.0F));
        ImGui::BeginDisabled(!playable || !theater.standalone);
        if (LinkButton("##watch", "Watch")) watch = summary.path;
        ImGui::EndDisabled();
        ImGui::SameLine(0.0F, 18.0F);
        if (LinkButton("##preview", "Preview")) open = summary.path;
        ImGui::SameLine(0.0F, 18.0F);
        // A file the recorder is still writing cannot be renamed or deleted.
        ImGui::BeginDisabled(partial && recording);
        if (LinkButton("##rename", "Rename")) {
            theater.dialog = Theater::Dialog::Rename;
            theater.dialogKey = key;
            theater.renameError.clear();
            theater.renameBuffer.fill('\0');
            if (!HasDefaultName(summary.path)) {
                const auto current = Narrow(summary.path.stem().wstring());
                std::copy_n(current.begin(), std::min(current.size(), theater.renameBuffer.size() - 1), theater.renameBuffer.begin());
            }
        }
        ImGui::SameLine(0.0F, 18.0F);
        if (LinkButton("##delete", "Delete")) {
            theater.dialog = Theater::Dialog::Delete;
            theater.dialogKey = key;
        }
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(position);
        ImGui::Dummy(ImVec2(cardWidth, CardHeight + ActionHeight));
        ImGui::EndGroup();
        if (clicked) {
            if (playable && theater.standalone) watch = summary.path;
            else open = summary.path;
        }
        ImGui::PopID();
        column = (column + 1) % columns;
        if (column == 0) ImGui::Dummy(ImVec2(0.0F, Gap - ImGui::GetStyle().ItemSpacing.y));
    }
    ImGui::EndChild();
    if (DrawReplayDialogs()) {
        EnterTheater();
        theater.phase = TheaterPhase::Library;
        return;
    }
    if (watch) WatchReplay(*watch);
    else if (open) OpenReplay(*open);
}

void SeekTo(double micros) {
    const double duration = theater.timeline ? static_cast<double>(theater.timeline->durationMicros) : 0.0;
    const double start = theater.timeline ? std::min(static_cast<double>(theater.timeline->clipStartMicros), duration) : 0.0;
    theater.playheadMicros = std::clamp(micros, start, duration);
}

void DrawTheaterViewer(float deltaSeconds) {
    if (theater.phase == TheaterPhase::ViewerLoading) {
        // Draw one spinner frame first, then load on the next one.
        if (theater.phaseSeconds > 0.0F) {
            theater.timeline = Replay::BuildTimeline(Replay::Read(theater.openPath));
            theater.drawPath = Replay::DecimatePath(*theater.timeline, 4000);
            theater.playing = !theater.timeline->samples.empty();
            theater.playheadMicros = static_cast<double>(theater.timeline->clipStartMicros);
            theater.phase = TheaterPhase::Viewer;
        } else {
            theater.phaseSeconds += std::max(deltaSeconds, 0.001F);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 area = ImGui::GetContentRegionAvail();
            const ImVec2 center(origin.x + area.x * 0.5F, origin.y + area.y * 0.45F);
            ImGui::GetWindowDrawList()->AddCircle(center, 18.0F, Pink, 32, 3.0F);
            DrawSnout(ImGui::GetWindowDrawList(), center, 0.6F);
            return;
        }
    }
    auto& timeline = *theater.timeline;
    const double duration = static_cast<double>(timeline.durationMicros);
    // A clip replays from its start; the session before it only rebuilds the world.
    const double clipStart = std::min(static_cast<double>(timeline.clipStartMicros), duration);
    const double clipLength = duration - clipStart;
    if (theater.playing) {
        theater.playheadMicros += static_cast<double>(deltaSeconds) * 1'000'000.0 * static_cast<double>(theater.speed);
        if (theater.playheadMicros >= duration) {
            theater.playheadMicros = duration;
            theater.playing = false;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !timeline.samples.empty()) {
        if (!theater.playing && theater.playheadMicros >= duration) theater.playheadMicros = clipStart;
        theater.playing = !theater.playing;
    }
    const auto playhead = static_cast<std::uint64_t>(theater.playheadMicros);

    if (LinkButton("##back", "< Replays")) {
        theater.phase = TheaterPhase::Library;
        return;
    }
    PageHeader(FormatStarted(timeline.startedUnixMillis).c_str(),
        Narrow(theater.openPath.filename().wstring()).c_str());
    ImGui::Dummy(ImVec2(0.0F, 4.0F));
    if (!timeline.error.empty()) {
        ImGui::TextColored(ToVec4(Amber), "Part of this replay is damaged: %s", timeline.error.c_str());
    }
    if (timeline.samples.empty()) {
        ImGui::TextColored(ToVec4(Secondary), "This replay has no recorded movement.");
        CaptionText(Muted, "Camera samples are recorded while you are in a world.");
        return;
    }

    constexpr float SidePanel = 170.0F;
    constexpr float TransportHeight = 74.0F;
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const ImVec2 mapPosition = ImGui::GetCursorScreenPos();
    const ImVec2 mapSize(std::max(120.0F, area.x - SidePanel - 12.0F), std::max(120.0F, area.y - TransportHeight));
    ImGui::InvisibleButton("##map", mapSize);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 mapMax(mapPosition.x + mapSize.x, mapPosition.y + mapSize.y);
    draw->AddRectFilled(mapPosition, mapMax, Rail, 8.0F);
    draw->PushClipRect(mapPosition, mapMax, true);
    const MapProjection project(mapPosition, mapSize, timeline.minX, timeline.maxX, timeline.minZ, timeline.maxZ, 24.0F);

    // Block grid at a step that keeps lines roughly 40 px or more apart.
    double step = 1.0;
    while (step * project.scale < 40.0 && step < 1'000'000.0) step *= step < 10.0 ? 2.0 : 5.0;
    const double gridStartX = std::floor((timeline.minX - project.offset.x / project.scale) / step) * step;
    const double gridStartZ = std::floor((timeline.minZ - project.offset.y / project.scale) / step) * step;
    for (double x = gridStartX; project(x, 0.0).x < mapMax.x; x += step) {
        const float screenX = project(x, 0.0).x;
        if (screenX >= mapPosition.x) draw->AddLine(ImVec2(screenX, mapPosition.y), ImVec2(screenX, mapMax.y), WithAlpha(Stroke, 0.45F));
    }
    for (double z = gridStartZ; project(0.0, z).y < mapMax.y; z += step) {
        const float screenY = project(0.0, z).y;
        if (screenY >= mapPosition.y) draw->AddLine(ImVec2(mapPosition.x, screenY), ImVec2(mapMax.x, screenY), WithAlpha(Stroke, 0.45F));
    }

    // Whole path dimmed, travelled part bright. Segments never join across
    // a dimension change.
    const auto drawSegments = [&](std::uint64_t until, ImU32 color, float thickness) {
        std::vector<ImVec2> run;
        std::optional<int> runDimension;
        const auto flush = [&] {
            if (run.size() >= 2) draw->AddPolyline(run.data(), static_cast<int>(run.size()), color, ImDrawFlags_None, thickness);
            run.clear();
        };
        const auto stride = std::max<std::size_t>(1, timeline.samples.size() / 4000);
        for (std::size_t index = 0; index < timeline.samples.size(); index += stride) {
            const auto& sample = timeline.samples[index];
            if (sample.t > until) break;
            if (!run.empty() && sample.dimension != runDimension) flush();
            runDimension = sample.dimension;
            run.push_back(project(sample.x, sample.z));
        }
        if (const auto now = Replay::SampleAt(timeline, until); now && now->dimension == runDimension) {
            run.push_back(project(now->x, now->z));
        }
        flush();
    };
    drawSegments(timeline.samples.back().t, WithAlpha(Muted, 0.35F), 2.0F);
    drawSegments(playhead, Pink, 2.6F);
    const auto& first = timeline.samples.front();
    draw->AddCircleFilled(project(first.x, first.z), 4.5F, Green);
    for (const auto& mark : timeline.marks) {
        if (!mark.label.starts_with("mark")) continue;
        const auto at = Replay::SampleAt(timeline, mark.t);
        if (!at) continue;
        const ImVec2 pin = project(at->x, at->z);
        draw->AddTriangleFilled(ImVec2(pin.x - 5.0F, pin.y - 12.0F), ImVec2(pin.x + 5.0F, pin.y - 12.0F), pin, Amber);
        draw->AddCircleFilled(ImVec2(pin.x, pin.y - 13.0F), 5.0F, Amber);
    }
    const auto current = Replay::SampleAt(timeline, playhead);
    if (current) {
        const ImVec2 player = project(current->x, current->z);
        draw->AddCircleFilled(player, 9.0F, WithAlpha(Pink, 0.25F));
        if (current->yaw) {
            // Bedrock yaw: 0 faces +Z (south, down on the map), 90 faces -X.
            const float radians = *current->yaw * 3.14159265F / 180.0F;
            const ImVec2 forward(-std::sin(radians), std::cos(radians));
            const ImVec2 side(-forward.y, forward.x);
            draw->AddTriangleFilled(ImVec2(player.x + forward.x * 11.0F, player.y + forward.y * 11.0F),
                ImVec2(player.x - forward.x * 6.0F + side.x * 7.0F, player.y - forward.y * 6.0F + side.y * 7.0F),
                ImVec2(player.x - forward.x * 6.0F - side.x * 7.0F, player.y - forward.y * 6.0F - side.y * 7.0F), Pink);
        } else {
            draw->AddCircleFilled(player, 6.0F, Pink);
        }
    }
    draw->AddText(CompactFont(), CompactFont()->FontSize, ImVec2(mapPosition.x + 10.0F, mapPosition.y + 8.0F), Muted,
        std::format("N ^   grid {} blocks", static_cast<long long>(step)).c_str());
    draw->PopClipRect();

    // Readout beside the map.
    ImGui::SameLine(0.0F, 12.0F);
    ImGui::BeginGroup();
    const auto readout = [](const char* label, const std::string& value) {
        CaptionText(Muted, label);
        ImGui::TextUnformatted(value.c_str());
        ImGui::Dummy(ImVec2(0.0F, 2.0F));
    };
    readout("TIME", FormatDuration(static_cast<std::uint64_t>(theater.playheadMicros - clipStart)) + " / "
        + FormatDuration(static_cast<std::uint64_t>(clipLength)));
    if (current) {
        readout("POSITION", std::format("{:.1f}  {:.1f}  {:.1f}", current->x, current->y, current->z));
        readout("FACING", current->yaw ? std::format("yaw {:.0f}  pitch {:.0f}", *current->yaw, current->pitch.value_or(0.0F))
            : std::string("not recorded"));
        readout("DIMENSION", DimensionName(current->dimension));
    }
    const auto highlights = std::ranges::count_if(timeline.marks,
        [](const Replay::TimelineMark& mark) { return mark.label.starts_with("mark"); });
    readout("HIGHLIGHTS", std::to_string(highlights));
    std::string firstIds;
    for (const auto id : timeline.firstPacketIds) firstIds += (firstIds.empty() ? "" : " ") + std::to_string(id);
    readout("WORLD DATA", timeline.packetCount == 0 ? std::string("none captured")
        : std::format("{} packets{}", timeline.packetCount, timeline.hasStartGame ? "" : ", no StartGame"));
    if (!firstIds.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 166.0F);
        ImGui::PushFont(CompactFont());
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(Muted));
        ImGui::TextWrapped("first ids %s", firstIds.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
    }
    CaptionText(Muted, "Space plays / pauses.");
    CaptionText(Muted, "Path preview. In-world playback is next.");
    ImGui::EndGroup();

    // Transport: jump between highlights, play, speed, and the scrubber.
    ImGui::Dummy(ImVec2(0.0F, 4.0F));
    const auto jump = [&](bool forward) {
        std::optional<std::uint64_t> target;
        for (const auto& mark : timeline.marks) {
            if (!mark.label.starts_with("mark")) continue;
            if (forward && mark.t > playhead + 1000) { target = mark.t; break; }
            if (!forward && mark.t + 1000 < playhead) target = mark.t;
        }
        if (target) SeekTo(static_cast<double>(*target));
        else SeekTo(forward ? duration : clipStart);
    };
    if (ImGui::Button("|<", ImVec2(36.0F, 28.0F))) jump(false);
    ImGui::SameLine();
    const bool atEnd = theater.playheadMicros >= duration;
    if (PrimaryButton(theater.playing ? "Pause" : atEnd ? "Replay" : "Play", ImVec2(76.0F, 28.0F))) {
        if (atEnd) theater.playheadMicros = clipStart;
        theater.playing = !theater.playing || atEnd;
    }
    ImGui::SameLine();
    if (ImGui::Button(">|", ImVec2(36.0F, 28.0F))) jump(true);
    constexpr std::array<float, 4> Speeds{0.5F, 1.0F, 2.0F, 4.0F};
    for (const float option : Speeds) {
        ImGui::SameLine();
        const bool selected = theater.speed == option;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ToVec4(PinkDeep));
        if (ImGui::Button(std::format("{}x##speed", option).c_str(), ImVec2(42.0F, 28.0F))) theater.speed = option;
        if (selected) ImGui::PopStyleColor();
    }

    const ImVec2 trackPosition = ImGui::GetCursorScreenPos();
    const float trackWidth = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##scrubber", ImVec2(trackWidth, 22.0F));
    if (ImGui::IsItemActive() && trackWidth > 0.0F) {
        const float fraction = std::clamp((ImGui::GetIO().MousePos.x - trackPosition.x) / trackWidth, 0.0F, 1.0F);
        SeekTo(clipStart + static_cast<double>(fraction) * clipLength);
    }
    const float trackY = trackPosition.y + 11.0F;
    const float fraction = clipLength > 0.0 ? static_cast<float>((theater.playheadMicros - clipStart) / clipLength) : 0.0F;
    draw->AddRectFilled(ImVec2(trackPosition.x, trackY - 2.5F), ImVec2(trackPosition.x + trackWidth, trackY + 2.5F), Stroke, 2.5F);
    draw->AddRectFilled(ImVec2(trackPosition.x, trackY - 2.5F),
        ImVec2(trackPosition.x + trackWidth * fraction, trackY + 2.5F), Pink, 2.5F);
    for (const auto& mark : timeline.marks) {
        if (!mark.label.starts_with("mark") || clipLength <= 0.0 || static_cast<double>(mark.t) < clipStart) continue;
        const float x = trackPosition.x + trackWidth * static_cast<float>((static_cast<double>(mark.t) - clipStart) / clipLength);
        draw->AddRectFilled(ImVec2(x - 1.5F, trackY - 8.0F), ImVec2(x + 1.5F, trackY + 8.0F), Amber, 1.0F);
    }
    draw->AddCircleFilled(ImVec2(trackPosition.x + trackWidth * fraction, trackY),
        ImGui::IsItemHovered() || ImGui::IsItemActive() ? 8.0F : 6.5F, Text);
}

void DrawReplaysPage(ModuleManager& modules, const FrameInfo& frame, bool standalone) {
    // Re-entering the page (or reopening the menu) skipped at least one frame.
    if (theater.lastFrame == 0 || frame.frameIndex != theater.lastFrame + 1 || theater.standalone != standalone) {
        theater.standalone = standalone;
        theater.closeRequested = false;
        EnterTheater();
    }
    theater.lastFrame = frame.frameIndex;
    switch (theater.phase) {
    case TheaterPhase::Intro: DrawTheaterIntro(frame.deltaSeconds); break;
    case TheaterPhase::Library: DrawTheaterLibrary(modules, frame.presentedAt); break;
    case TheaterPhase::ViewerLoading:
    case TheaterPhase::Viewer: DrawTheaterViewer(frame.deltaSeconds); break;
    }
}

// Esc inside the viewer returns to the library instead of leaving the page.
bool TheaterConsumesEscape() {
    if (!theater.standalone) return false;
    if (theater.dialog != Theater::Dialog::None) {
        theater.dialog = Theater::Dialog::None;
        return true;
    }
    if (theater.phase != TheaterPhase::Viewer && theater.phase != TheaterPhase::ViewerLoading) return false;
    theater.phase = TheaterPhase::Library;
    theater.playing = false;
    return true;
}

std::filesystem::path ProfileDirectory() {
    wchar_t localAppData[32768]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
        static_cast<DWORD>(std::size(localAppData)));
    if (length > 0 && length < std::size(localAppData)) {
        return std::filesystem::path(localAppData) / L"Oinkster" / L"Client" / L"profiles";
    }
    return std::filesystem::temp_directory_path() / L"Oinkster" / L"profiles";
}

void RefreshProfiles() {
    const SettingsProfileStore store(ProfileDirectory());
    profileNames = store.List();
    activeProfile = store.Active();
    if (!activeProfile.empty()
        && std::ranges::find(profileNames, activeProfile) == profileNames.end()) activeProfile.clear();
    profilesLoaded = true;
}

ClientSettings CaptureCurrentProfile(const ModuleManager& modules) {
    return CaptureSettings(modules, Dx12Overlay::MenuKey(), Dx12Overlay::UnloadKey(), 0);
}

void DrawProfilesPage(ModuleManager& modules, bool& dirty) {
    if (!profilesLoaded) RefreshProfiles();
    PageHeader("Profiles", "Save and switch complete module, keybind, and HUD setups.");
    ImGui::Dummy(ImVec2(0.0F, 10.0F));

    CaptionText(Muted, activeProfile.empty() ? "No active profile" : ("Active: " + activeProfile).c_str());
    if (!activeProfile.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Save current")) {
            try {
                SettingsProfileStore(ProfileDirectory()).Save(activeProfile, CaptureCurrentProfile(modules));
                profileMessage = "Profile updated.";
                profileMessageIsError = false;
            } catch (const std::exception& error) {
                profileMessage = error.what();
                profileMessageIsError = true;
            }
        }
    }

    GroupLabel("New profile");
    ImGui::SetNextItemWidth(std::min(280.0F, ImGui::GetContentRegionAvail().x - 110.0F));
    const bool submitted = ImGui::InputTextWithHint("##new-profile", "Profile name", newProfileName.data(), newProfileName.size(),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (submitted || ImGui::Button("Create", ImVec2(92.0F, 0.0F))) {
        const std::string name(newProfileName.data());
        if (!SettingsProfileStore::IsValidName(name)) {
            profileMessage = "Use 1-32 letters, numbers, spaces, dashes, or underscores.";
            profileMessageIsError = true;
        } else if (std::ranges::find(profileNames, name) != profileNames.end()) {
            profileMessage = "A profile with that name already exists.";
            profileMessageIsError = true;
        } else {
            try {
                const SettingsProfileStore store(ProfileDirectory());
                store.Save(name, CaptureCurrentProfile(modules));
                store.SetActive(name);
                newProfileName.fill('\0');
                profileMessage = "Profile created.";
                profileMessageIsError = false;
                RefreshProfiles();
            } catch (const std::exception& error) {
                profileMessage = error.what();
                profileMessageIsError = true;
            }
        }
    }
    if (!profileMessage.empty()) CaptionText(profileMessageIsError ? Amber : Green, profileMessage.c_str());

    GroupLabel("Saved profiles");
    if (profileNames.empty()) {
        CaptionText(Muted, "Create a profile to save your current setup.");
        return;
    }

    for (const auto& name : profileNames) {
        ImGui::PushID(name.c_str());
        const bool active = name == activeProfile;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ToVec4(active ? Pink : Text), "%s", name.c_str());
        const float actionsWidth = pendingDeleteProfile == name ? 184.0F : 132.0F;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0F, ImGui::GetWindowContentRegionMax().x - actionsWidth));
        if (!active) {
            if (ImGui::Button("Load", ImVec2(60.0F, 0.0F))) {
                try {
                    const SettingsProfileStore store(ProfileDirectory());
                    if (!activeProfile.empty()) store.Save(activeProfile, CaptureCurrentProfile(modules));
                    const auto settings = store.Load(name);
                    if (!settings) throw std::runtime_error("Profile could not be loaded");
                    ApplySettings(modules, *settings);
                    Dx12Overlay::ConfigureControlKeys(settings->clickGuiKey, settings->unloadKey);
                    store.SetActive(name);
                    activeProfile = name;
                    pendingDeleteProfile.clear();
                    profileMessage = "Loaded " + name + ".";
                    profileMessageIsError = false;
                    dirty = true;
                } catch (const std::exception& error) {
                    profileMessage = error.what();
                    profileMessageIsError = true;
                }
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("Active", ImVec2(60.0F, 0.0F));
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (pendingDeleteProfile == name) {
            if (ImGui::Button("Delete", ImVec2(60.0F, 0.0F))) {
                const SettingsProfileStore store(ProfileDirectory());
                if (store.Remove(name)) {
                    if (active) store.SetActive({});
                    profileMessage = "Profile deleted.";
                    profileMessageIsError = false;
                } else {
                    profileMessage = "Profile could not be deleted.";
                    profileMessageIsError = true;
                }
                pendingDeleteProfile.clear();
                RefreshProfiles();
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(60.0F, 0.0F))) pendingDeleteProfile.clear();
        } else if (ImGui::Button("Remove", ImVec2(64.0F, 0.0F))) {
            pendingDeleteProfile = name;
        }
        ImGui::PopID();
    }
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
    if (NavItem("##nav-profiles", "Profiles", std::nullopt, section == Section::Profiles)) section = Section::Profiles;
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

bool HasTransientUi() noexcept { return sessionActive.load() || ReplayInjector::Running(); }

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

// A Bedrock-style menu button (black outline, light face, dark lip that the
// face sinks into while pressed) in the main menu's top-left corner.
bool DrawReplaysMenuButton() {
    const auto display = ImGui::GetIO().DisplaySize;
    const float scale = std::clamp(std::min(display.x / 1280.0F, display.y / 720.0F), 0.8F, 1.6F);
    const ImVec2 size(std::round(150.0F * scale), std::round(40.0F * scale));
    const ImVec2 position(std::round(16.0F * scale), std::round(16.0F * scale));
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::Begin("##main-menu-replays", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav);
    const bool pressed = ImGui::InvisibleButton("##replays-button", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    auto* draw = ImGui::GetWindowDrawList();
    const float border = std::max(2.0F, std::round(2.0F * scale));
    const float fullLip = std::round(5.0F * scale);
    const float lip = held ? std::round(2.0F * scale) : fullLip;
    const ImVec2 maximum(position.x + size.x, position.y + size.y);
    draw->AddRectFilled(position, maximum, IM_COL32(0, 0, 0, 255));
    const ImVec2 innerMin(position.x + border, position.y + border + (fullLip - lip));
    const ImVec2 innerMax(maximum.x - border, maximum.y - border);
    draw->AddRectFilled(innerMin, innerMax, IM_COL32(88, 88, 90, 255));
    const ImU32 face = held ? IM_COL32(176, 177, 180, 255)
        : hovered ? IM_COL32(234, 235, 238, 255) : IM_COL32(208, 209, 212, 255);
    const ImVec2 faceMax(innerMax.x, innerMax.y - lip);
    draw->AddRectFilled(innerMin, faceMax, face);
    // Light bevel along the top of the face, like the game's own buttons.
    draw->AddRectFilled(innerMin, ImVec2(faceMax.x, innerMin.y + border),
        IM_COL32(255, 255, 255, hovered ? 200 : 120));
    if (hovered && !held) {
        draw->AddRect(position, maximum, IM_COL32(255, 255, 255, 255), 0.0F, 0, border);
    }
    ImFont* font = TitleFont();
    const char* label = "Replays";
    const ImVec2 textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F, label);
    const ImVec2 text(std::round(innerMin.x + (faceMax.x - innerMin.x - textSize.x) * 0.5F),
        std::round(innerMin.y + (faceMax.y - innerMin.y - textSize.y) * 0.5F));
    draw->AddText(font, font->FontSize, ImVec2(text.x + 1.0F, text.y + 1.0F), IM_COL32(255, 255, 255, 110), label);
    draw->AddText(font, font->FontSize, text, IM_COL32(49, 50, 51, 255), label);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::End();
    ImGui::PopStyleVar(2);
    return pressed;
}

// The theater on its own, opened from the main-menu button.
void DrawStandaloneTheater(ModuleManager& modules, const FrameInfo& frame, DrawResult& result) {
    const auto display = ImGui::GetIO().DisplaySize;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !TheaterConsumesEscape()) result.closeRequested = true;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0F, 0.0F), display, IM_COL32(0, 0, 0, 150));
    const ImVec2 size(std::min(std::clamp(display.x * 0.62F, 760.0F, 1100.0F), std::max(320.0F, display.x - 24.0F)),
        std::min(std::clamp(display.y * 0.76F, 520.0F, 800.0F), std::max(320.0F, display.y - 24.0F)));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5F, display.y * 0.5F), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0F, 20.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0F, 6.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0F, 6.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10.0F);
    constexpr int StyleVars = 8;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(Canvas));
    ImGui::PushStyleColor(ImGuiCol_Border, ToVec4(WithAlpha(Pink, 0.35F)));
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(Text));
    constexpr int StyleColors = 3;
    ImGui::Begin("##replay-theater", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse);
    DrawReplaysPage(modules, frame, true);
    ImGui::End();
    ImGui::PopStyleColor(StyleColors);
    ImGui::PopStyleVar(StyleVars);
    if (theater.closeRequested) {
        theater.closeRequested = false;
        result.closeRequested = true;
    }
}

DrawResult Draw(ModuleManager& modules, const DrawMode& mode, bool& hudEditMode, const FrameInfo& frame) {
    const bool menuVisible = mode.menuVisible;
    const bool gameplayHudVisible = mode.gameplayHudVisible;
    DrawResult result{};
    const auto display = ImGui::GetIO().DisplaySize;
    if (moduleBrowserOwner != &modules) {
        moduleBrowser = std::make_unique<ClickGui>(modules);
        moduleBrowserOwner = &modules;
        profilesLoaded = false;
    }
    const float deltaSeconds = frame.deltaSeconds;
    modules.Publish(RenderEvent{display.x, display.y, 96.0F, frame.deltaSeconds, menuVisible, hudEditMode,
        frame.gameFocused, frame.frameIndex, frame.presentedAt, frame.keysDown, gameplayHudVisible});
    DrawToast();
    if (mode.replayPanelVisible && !menuVisible) DrawReplayPanel(modules, frame.presentedAt);
    // While the camera flies a path, Oinkster's HUD stays out of the shot.
    bool cleanShot = false;
    if (ReplayInjector::Running() || Render::Busy()) {
        const auto status = ReplayInjector::CurrentStatus();
        Render::Update(status);
        cleanShot = status.cameraPathPlaying;
        if (status.running && !menuVisible && !mode.replayPanelVisible) {
            if (status.playing) DrawReplayHotbar(status, frame, result);
            else DrawReplayPill();
        }
    }
    Render::DrawBadge();
    if (mode.replaysButtonVisible && !menuVisible && DrawReplaysMenuButton()) result.openReplayTheater = true;
    if (!menuVisible && gameplayHudVisible && !cleanShot) DrawOverlayPrimitives(modules, display);
    if (!menuVisible && gameplayHudVisible && !cleanShot) DrawDebugOverlays(modules, display);
    if (!menuVisible && gameplayHudVisible && !cleanShot) DrawPlayerLists(modules, display);
    if (!menuVisible && !cleanShot) DrawModuleNotices(modules, display);
    if ((!menuVisible && gameplayHudVisible && !cleanShot) || hudEditMode) result.settingsDirty |= DrawHud(modules, hudEditMode);
    if (!menuVisible) {
        menuWasVisible = false;
        return result;
    }
    if (mode.replayEditor) {
        DrawReplayEditor(frame, result);
        return result;
    }
    if (mode.replayTheater) {
        DrawStandaloneTheater(modules, frame, result);
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
        else if (TheaterConsumesEscape()) {}
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
        DrawHomeHub(modules, deltaSeconds, hudEditMode);
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
    case Section::Profiles: DrawProfilesPage(modules, result.settingsDirty); break;
    case Section::Hud: DrawHudPage(modules, hudEditMode, result.settingsDirty); break;
    case Section::Settings: DrawSettingsPage(modules, result.settingsDirty); break;
    case Section::Home: break;
    }
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleColor(StyleColors);
    ImGui::PopStyleVar(StyleVars);
    return result;
}
}
