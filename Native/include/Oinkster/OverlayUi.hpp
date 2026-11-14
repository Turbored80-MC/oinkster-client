#pragma once

#include <bitset>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

struct ImFont;
namespace Oinkster { class ModuleManager; }

namespace Oinkster::OverlayUi {
struct DrawResult {
    bool settingsDirty{};
    bool closeRequested{};
    bool openReplayTheater{};
    // The replay editor's gear button: keep the menu open as the ClickGUI.
    bool openClickGui{};
    // The replay hotbar's editor slot: open the menu as the replay editor.
    bool openReplayEditor{};
};

// Which overlay surfaces the Present hook wants this frame.
struct DrawMode {
    bool menuVisible{};
    // The menu is open as the standalone Replays theater (opened from the
    // main-menu Replays button) rather than the ClickGUI.
    bool replayTheater{};
    bool gameplayHudVisible{};
    bool replayPanelVisible{};
    // Outside a world: the Minecraft-style Replays button, top left.
    bool replaysButtonVisible{};
    // The menu is open as the replay editor (a replay world is playing).
    bool replayEditor{};
};

// Per-Present timing measured by the Present hook (not ImGui's DeltaTime,
// which only advances on frames where the overlay draws).
struct FrameInfo {
    float deltaSeconds{};
    std::uint64_t frameIndex{};
    std::chrono::steady_clock::time_point presentedAt{};
    bool gameFocused{true};
    std::optional<std::bitset<256>> keysDown;
    // GPU descriptor (ImTextureID) of the last raw game frame, captured
    // while the replay editor is open; it is drawn in the editor's viewport.
    std::optional<std::uint64_t> gameFrameTexture;
    // Replay hotbar input since the last frame: wheel notches (positive =
    // up) and left clicks, taken from the game while a replay is watched.
    int replayHotbarScroll{};
    int replayHotbarClicks{};
    int replayHotbarRightClicks{};
};

void BeginSession() noexcept;
void ConfigureFonts(ImFont* compact, ImFont* body, ImFont* title) noexcept;
[[nodiscard]] bool HasTransientUi() noexcept;
[[nodiscard]] bool UpdatePlayerSkin(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgbaPixels, std::uint64_t revision) noexcept;
DrawResult Draw(ModuleManager& modules, const DrawMode& mode, bool& hudEditMode, const FrameInfo& frame);
}
