#pragma once

#include "Capabilities.hpp"
#include "Events.hpp"
#include "ModuleSettings.hpp"
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Oinkster {

enum class ModuleCategory { Performance, Input, Status, Telemetry, PvpFeedback, Visual, Accessibility };
enum class EventPriority { Immediate, High, Normal, Low, Last };

struct ModuleMetadata {
    std::string id;
    std::wstring name;
    std::wstring description;
    ModuleCategory category{};
    bool defaultEnabled{};
    // Filled in by ModuleManager::Catalog() from the capability registry;
    // ignored on a module's own Metadata().
    bool available{true};
    EventPriority eventPriority{EventPriority::Normal};
    std::wstring unavailableReason;
    bool hud{};
    // Availability is exactly "every one of these is Available".
    std::vector<Capability> requiredCapabilities;
};

struct HudLayout {
    float normalizedX{0.02F};
    float normalizedY{0.05F};
    float scale{1.0F};
    bool background{true};
    float backgroundOpacity{0.88F};
    std::uint32_t backgroundColor{0xFF111417U};
    std::uint32_t textColor{0xFFE8ECEFU};
    std::uint32_t accentColor{0xFFF28FB5U};
};

[[nodiscard]] HudLayout ClampHudLayout(HudLayout value) noexcept;

// What a HUD module wants drawn. Pure data so modules stay ImGui-free and
// testable; OverlayUi renders it.
enum class HudTone : std::uint8_t { Normal, Accent, Muted, Warning, Good };

struct HudLine {
    std::string text;  // UTF-8
    HudTone tone{HudTone::Normal};
};

struct HudGraph {
    std::vector<float> values;  // chronological, oldest first
    float minimum{};
    float maximum{1.0F};
    std::string caption;
};

struct HudKey {
    std::string label;
    bool pressed{};
    // Columns this key spans. Every row of a key grid should sum to the same
    // total so the renderer can lay rows out flush with each other.
    float widthUnits{1.0F};
    std::string subLabel;  // smaller second line, e.g. "7 CPS"
    bool spacer{};         // invisible cell, used to center a narrower row
    bool bar{};            // drawn as a short horizontal bar (space bar) instead of its label
};

struct HudContent {
    std::vector<HudLine> lines;
    std::optional<HudGraph> graph;
    std::vector<std::vector<HudKey>> keyRows;
    // Draw without the HUD window's background panel (key grids supply their own tiles).
    bool bare{};
};

struct ModuleAction {
    std::string id;
    std::string label;
};

class IModule {
public:
    virtual ~IModule() = default;
    // Identity only, immutable after construction (read without locking).
    [[nodiscard]] virtual const ModuleMetadata& Metadata() const noexcept = 0;

    // Declared settings. ModuleManager normalizes stored values against
    // these and calls OnSettingsChanged with a fresh reader on every change
    // (and once, with defaults, at registration).
    [[nodiscard]] virtual std::span<const SettingDescriptor> Settings() const noexcept { return {}; }
    virtual void OnSettingsChanged(const SettingsReader&) {}

    // One-shot commands surfaced as buttons in the ClickGUI (e.g. stopwatch
    // start/pause/reset). Returns true if the action was recognized.
    [[nodiscard]] virtual std::vector<ModuleAction> Actions() const { return {}; }
    virtual bool InvokeAction(std::string_view, SteadyTime) { return false; }

    virtual void OnEnable() {}
    virtual void OnDisable() {}
    virtual void OnTick(const TickEvent&) {}
    virtual void OnRender(const RenderEvent&) {}
    virtual void OnKeyboard(const KeyboardEvent&) {}
    virtual void OnMouse(const MouseEvent&) {}
    virtual void OnPlayerTransform(const PlayerTransform&) {}
    virtual void OnEquipment(const EquipmentSnapshot&) {}
    virtual void OnInventory(const InventorySnapshot&) {}
    virtual void OnEffects(const EffectsSnapshot&) {}
    virtual void OnNetworkMetrics(const NetworkMetrics&) {}
    virtual void OnServerState(const ServerState&) {}
    virtual void OnResourcePacks(const ResourcePackState&) {}
    virtual void OnCombat(const CombatEvent&) {}
    virtual void OnSound(const SoundEvent&) {}
    virtual void OnChat(const ChatEvent&) {}
};

class IHudModule : public IModule {
public:
    [[nodiscard]] virtual HudContent Content() const = 0;
    [[nodiscard]] virtual HudLayout Layout() const noexcept = 0;
    virtual void SetLayout(HudLayout value) noexcept = 0;
    [[nodiscard]] virtual HudLayout DefaultLayout() const noexcept = 0;
    // All lines joined with "\n" - convenient for tests and diagnostics.
    [[nodiscard]] std::string DisplayText() const;
};

// ----------------------------------------------------------------------
// Optional integration mixins. Integration code (Dx12Overlay, OverlayUi)
// finds enabled modules by these interfaces - never by module id.
// ----------------------------------------------------------------------

struct OverlayLine { float x1{}, y1{}, x2{}, y2{}; std::uint32_t argb{}; float thickness{1.0F}; };
struct OverlayRect { float x1{}, y1{}, x2{}, y2{}; std::uint32_t argb{}; bool filled{true}; float thickness{1.0F}; };
struct OverlayCircle { float x{}, y{}, radius{}; std::uint32_t argb{}; bool filled{true}; float thickness{1.0F}; };

struct OverlayPrimitives {
    std::vector<OverlayRect> rects;
    std::vector<OverlayLine> lines;
    std::vector<OverlayCircle> circles;
    [[nodiscard]] bool Empty() const noexcept { return rects.empty() && lines.empty() && circles.empty(); }
};

// Draws simple screen-space geometry under the HUD while gameplay is visible.
class IOverlayModule {
public:
    virtual ~IOverlayModule() = default;
    [[nodiscard]] virtual OverlayPrimitives Primitives(float width, float height) const = 0;
};

// Throttles presentation while the game is in the background.
class IFramePacingModule {
public:
    virtual ~IFramePacingModule() = default;
    // Minimum time between presented frames, or nullopt for no limit.
    [[nodiscard]] virtual std::optional<std::chrono::microseconds> FrameInterval(bool focused, bool minimized) const = 0;
};

struct SprintInputs {
    bool forwardHeld{};
    bool sneakHeld{};
    bool gameplayHasCursor{};   // false while a game screen (chat, inventory) owns the cursor
    bool menuVisible{};
    bool focused{};
};

// Holds the game's own sprint key on the player's behalf, through the same
// window-message path as an ordinary key press. Never exceeds what a player
// could do by holding that key.
class ISprintAssistModule {
public:
    virtual ~ISprintAssistModule() = default;
    [[nodiscard]] virtual bool WantsSprintHeld(const SprintInputs& inputs) = 0;
    [[nodiscard]] virtual std::uint32_t SprintKey() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t ForwardKey() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t SneakKey() const noexcept = 0;
};

// Cosmetic blend of the previous presented frame over the current one.
class IFrameBlendModule {
public:
    virtual ~IFrameBlendModule() = default;
    // 0 = disabled; always <= MaxFrameBlendStrength.
    [[nodiscard]] virtual float BlendStrength() const noexcept = 0;
};

inline constexpr float MaxFrameBlendStrength = 0.85F;

}
