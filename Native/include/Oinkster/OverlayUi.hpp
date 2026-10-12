#pragma once

#include <bitset>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

struct ImFont;
namespace Oinkster { class ModuleManager; }

namespace Oinkster::OverlayUi {
struct DrawResult { bool settingsDirty{}; bool closeRequested{}; };

// Per-Present timing measured by the Present hook (not ImGui's DeltaTime,
// which only advances on frames where the overlay draws).
struct FrameInfo {
    float deltaSeconds{};
    std::uint64_t frameIndex{};
    std::chrono::steady_clock::time_point presentedAt{};
    bool gameFocused{true};
    std::optional<std::bitset<256>> keysDown;
};

void BeginSession() noexcept;
void ConfigureFonts(ImFont* compact, ImFont* body, ImFont* title) noexcept;
[[nodiscard]] bool HasTransientUi() noexcept;
[[nodiscard]] bool UpdatePlayerSkin(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgbaPixels, std::uint64_t revision) noexcept;
DrawResult Draw(ModuleManager& modules, bool menuVisible, bool gameplayHudVisible, bool& hudEditMode, const FrameInfo& frame);
}
