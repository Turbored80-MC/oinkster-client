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

// Java-style debug information is anchored to both screen edges instead of
// participating in the draggable HUD layout. Groups create the small vertical
// gaps used by Minecraft's F3 overlay; rendering stays in OverlayUi so modules
// remain independent of ImGui.
struct DebugOverlayGroup {
    std::vector<HudLine> lines;
};

struct DebugOverlayContent {
    bool visible{};
    float scale{1.0F};
    std::vector<DebugOverlayGroup> left;
    std::vector<DebugOverlayGroup> right;
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
    virtual void OnCameraPerspective(const CameraPerspectiveEvent&) {}
    virtual void OnGameCrosshair(const GameCrosshairEvent&) {}
    virtual void OnBlockSelection(const BlockSelectionEvent&) {}
    virtual void OnCameraPose(const CameraPoseEvent&) {}
    virtual void OnReplayPacket(const ReplayPacketEvent&) {}
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
// A filled convex quad, corners in order.
struct OverlayQuad { std::array<float, 4> x{}; std::array<float, 4> y{}; std::uint32_t argb{}; };

struct OverlayPrimitives {
    std::vector<OverlayRect> rects;
    std::vector<OverlayLine> lines;
    std::vector<OverlayCircle> circles;
    std::vector<OverlayQuad> quads;
    [[nodiscard]] bool Empty() const noexcept { return rects.empty() && lines.empty() && circles.empty() && quads.empty(); }
};

// Draws simple screen-space geometry under the HUD while gameplay is visible.
class IOverlayModule {
public:
    virtual ~IOverlayModule() = default;
    [[nodiscard]] virtual OverlayPrimitives Primitives(float width, float height) const = 0;
};

class IDebugOverlayModule {
public:
    virtual ~IDebugOverlayModule() = default;
    [[nodiscard]] virtual DebugOverlayContent DebugOverlay() const = 0;
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

struct HeldKeyInputs {
    bool gameplayHasCursor{};
    bool menuVisible{};
    bool focused{};
};

// Requests one ordinary game key be held on the player's behalf. The DX12
// integration owns the actual key-up bookkeeping, so disabling a module,
// opening a screen, losing focus, or unloading can never strand a key down.
class IKeyHoldAssistModule {
public:
    virtual ~IKeyHoldAssistModule() = default;
    [[nodiscard]] virtual std::uint32_t DesiredHeldKey(const HeldKeyInputs& inputs) = 0;
    virtual void ResetHeldKeyState() noexcept = 0;
};

// Cosmetic blend of the previous presented frame over the current one.
class IFrameBlendModule {
public:
    virtual ~IFrameBlendModule() = default;
    // 0 = disabled; always <= MaxFrameBlendStrength.
    [[nodiscard]] virtual float BlendStrength() const noexcept = 0;
};

enum class ReplayControlState : std::uint8_t { Idle, Recording, Paused, Error };

struct ReplayControlSnapshot {
    ReplayControlState state{ReplayControlState::Idle};
    std::chrono::milliseconds elapsed{};
    std::size_t markers{};
    std::string detail;
    // Start is only possible while a world session is being captured from
    // its join; replays cannot be built from mid-session.
    bool canStart{};
};

// Supplies the recorder controls shown beside Minecraft's pause menu and on
// the ClickGUI Replays page. The module owns recording and file lifetime; the
// overlay only renders commands.
class IReplayControlModule {
public:
    virtual ~IReplayControlModule() = default;
    [[nodiscard]] virtual ReplayControlSnapshot ReplaySnapshot(SteadyTime now) const = 0;
    virtual bool StartReplay(SteadyTime now) = 0;
    virtual bool ToggleReplayPause(SteadyTime now) = 0;
    virtual bool MarkReplay(SteadyTime now) = 0;
    virtual bool FinishReplay(SteadyTime now) = 0;
    virtual bool DiscardReplay() = 0;
};

inline constexpr float MaxFrameBlendStrength = 0.85F;

// Reads specific server->client packets (the player list, chat, forms).
// ReplayPacketBridge copies a packet into a ReplayPacketEvent only when an
// enabled module asks for its id, so ordinary play costs nothing.
class IPacketObserverModule {
public:
    virtual ~IPacketObserverModule() = default;
    [[nodiscard]] virtual bool WantsPacket(std::uint32_t packetId) const noexcept = 0;
};

// A Java-style tab list: the server's player list while a key is held.
struct PlayerListRow {
    std::string name;                     // UTF-8, colour codes removed
    std::array<std::uint32_t, 64> face{};  // 8x8 ARGB head front
    bool hasFace{};
};

struct PlayerListView {
    bool visible{};
    std::string title;   // server or world name, may be empty
    std::vector<PlayerListRow> rows;
    bool heads{true};
    int columns{};       // 0 = automatic
    float scale{1.0F};
};

// Short confirmations ("Requeued SkyWars", "Saved Steve's skin") shown as a
// toast at the top of the screen. Drained once per frame.
class INoticeModule {
public:
    virtual ~INoticeModule() = default;
    [[nodiscard]] virtual std::vector<std::string> TakeNotices() = 0;
};

class IPlayerListModule {
public:
    virtual ~IPlayerListModule() = default;
    [[nodiscard]] virtual PlayerListView PlayerList() const = 0;
};

// How the local mouse reaches gameplay. Applied by MouseInputBlocker on the
// game's own GameInput reading and MouseDevice::feed, so it never touches
// the system cursor or other applications.
struct MouseShaping {
    bool blockWheel{};          // swallow wheel notches while gameplay owns the mouse
    float sensitivityX{1.0F};   // multiplier on horizontal look movement
    float sensitivityY{1.0F};
};

class IMouseShapingModule {
public:
    virtual ~IMouseShapingModule() = default;
    // Called once per presented frame; the module adjusts what it owns.
    virtual void ShapeMouse(MouseShaping& shaping, bool gameplayHasCursor) const = 0;
};

// One key message to hand to the game in place of a physical one.
struct KeyMessage {
    std::uint32_t key{};
    bool pressed{};
};

// Rewrites the game's keyboard stream on the window thread (e.g. Null
// Movement: of two opposing keys only the newest stays down). Only ordinary
// key-down/key-up messages are emitted, never more keys than are physically
// held.
class IKeyTransformModule {
public:
    virtual ~IKeyTransformModule() = default;
    // Returns true when the module took the message; `out` then replaces it
    // (possibly empty). `physical` is the real held state (GetAsyncKeyState).
    [[nodiscard]] virtual bool TransformKey(std::uint32_t key, bool pressed, bool repeated,
        const std::array<bool, 256>& physical, std::vector<KeyMessage>& out) = 0;
    // Gameplay lost the keyboard (menu, game screen, focus): forget state.
    virtual void ResetKeyTransform() noexcept = 0;
};

// Re-presses keys the player is still physically holding when a game screen
// (inventory, chat, pause) closes, since Bedrock drops them.
class IHeldKeyRestoreModule {
public:
    virtual ~IHeldKeyRestoreModule() = default;
    [[nodiscard]] virtual std::vector<std::uint32_t> KeysToRestore() const = 0;
};

// A cosmetic colour grade drawn over the presented frame.
struct PostProcessSettings {
    float saturation{1.0F};    // 0 = greyscale, 1 = unchanged, 2 = vivid
    float contrast{1.0F};
    float brightness{};        // -0.5 .. 0.5 added to every channel
    float vignette{};          // 0 .. 1 corner darkening
    float sharpen{};           // 0 .. 1 unsharp mask
    std::uint32_t tint{0xFFFFFFFFU};
    float tintStrength{};      // 0 .. 1
    [[nodiscard]] bool Identity() const noexcept {
        return saturation == 1.0F && contrast == 1.0F && brightness == 0.0F && vignette == 0.0F
            && sharpen == 0.0F && tintStrength == 0.0F;
    }
};

class IPostProcessModule {
public:
    virtual ~IPostProcessModule() = default;
    [[nodiscard]] virtual PostProcessSettings PostProcess() const = 0;
};

}
