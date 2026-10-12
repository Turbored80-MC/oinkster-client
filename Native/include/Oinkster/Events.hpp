#pragma once

#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Typed, read-only inputs to modules. Everything here is a plain value: no
// raw addresses, no pointers into Minecraft, no mutable game objects. Adapters
// build these from validated reads and hand them to ModuleManager::Publish;
// modules only ever see a const reference.
//
// Every snapshot carries the steady-clock time it was captured, so modules
// can do rate/staleness math deterministically (and tests can drive them
// with explicit timestamps instead of sleeping).
namespace Oinkster {

using SteadyClock = std::chrono::steady_clock;
using SteadyTime = SteadyClock::time_point;

struct RenderEvent {
    float width{};
    float height{};
    float dpi{96.0F};
    float deltaSeconds{};
    bool menuVisible{};
    bool hudEditMode{};
    // True while a Minecraft-owned window is the foreground window.
    bool gameFocused{true};
    // Monotonic per presented frame. Shared frame history deduplicates on it
    // so several enabled performance modules never double-count a frame.
    std::uint64_t frameIndex{};
    SteadyTime presentedAt{};
    // Physical key/button state (GetAsyncKeyState, indexed by VK code) sampled
    // at Present while the game is focused; empty when unfocused or in tests.
    // Minecraft reads gameplay input through raw input / GameInput, so window
    // messages alone miss presses while the cursor is captured.
    std::optional<std::bitset<256>> keysDown;
};

struct TickEvent { SteadyClock::duration elapsed{}; };

struct KeyboardEvent {
    std::uint32_t key{};
    bool pressed{};
    bool repeated{};
    // True when the ClickGUI owns input (the event never reaches gameplay).
    bool uiCaptured{};
    SteadyTime occurredAt{SteadyClock::now()};
    mutable bool cancelled{};
    void Cancel() const noexcept { cancelled = true; }
    [[nodiscard]] bool IsCancelled() const noexcept { return cancelled; }
};

// changedButton / buttons bits: 1 left, 2 right, 4 middle, 8 X1, 16 X2.
struct MouseEvent {
    float x{};
    float y{};
    float wheel{};
    std::uint32_t buttons{};
    std::uint32_t changedButton{};
    bool pressed{};
    bool uiCaptured{};
    SteadyTime occurredAt{SteadyClock::now()};
    mutable bool cancelled{};
    void Cancel() const noexcept { cancelled = true; }
    [[nodiscard]] bool IsCancelled() const noexcept { return cancelled; }
};

struct Vec3 {
    double x{};
    double y{};
    double z{};
};

// Local player transform. Rotation is optional because the currently
// validated telemetry (camera position) has no yaw/pitch source; modules
// that need rotation require Capability::LocalPlayerRotation separately.
//
// Bedrock yaw convention: 0 = south (+Z), 90 = west (-X), 180 = north (-Z),
// 270/-90 = east (+X).
struct PlayerTransform {
    SteadyTime capturedAt{};
    Vec3 position{};
    std::optional<float> yawDegrees;
    std::optional<float> pitchDegrees;
    // Changes when the player changes dimension; movement speed resets on it.
    std::optional<int> dimension;
};

struct ItemStack {
    std::string itemId;          // namespaced, e.g. "minecraft:diamond_sword"
    std::wstring displayName;
    int count{};
    std::optional<int> durabilityRemaining;
    std::optional<int> durabilityMax;
};

enum class ArmorSlot : std::uint8_t { Head, Chest, Legs, Feet };
inline constexpr std::size_t ArmorSlotCount = 4;

struct EquipmentSnapshot {
    SteadyTime capturedAt{};
    std::array<std::optional<ItemStack>, ArmorSlotCount> armor{};
    std::optional<ItemStack> mainHand;
    std::optional<ItemStack> offHand;
};

struct InventorySnapshot {
    SteadyTime capturedAt{};
    // Consumers bound this (see Modules::MaxInventorySlots); an adapter
    // must never hand over an unbounded, memory-derived length.
    std::vector<std::optional<ItemStack>> slots;
    int selectedHotbarSlot{};
};

struct StatusEffect {
    std::string effectId;        // e.g. "minecraft:speed"
    std::wstring displayName;
    int amplifier{};             // 0 = level I
    std::optional<std::chrono::milliseconds> remaining;  // nullopt = infinite
};

struct EffectsSnapshot {
    SteadyTime capturedAt{};
    std::vector<StatusEffect> effects;
};

struct NetworkMetrics {
    SteadyTime capturedAt{};
    std::optional<std::uint32_t> pingMilliseconds;
};

enum class SessionKind : std::uint8_t { None, Singleplayer, Lan, Realm, Server };

// Identity only - an adapter must never put an address, port, or credential
// here. ServerDisplay additionally masks anything address-shaped.
struct ServerState {
    SteadyTime capturedAt{};
    SessionKind kind{SessionKind::None};
    std::wstring displayName;
};

struct ResourcePackInfo {
    std::wstring name;
    std::wstring version;
};

struct ResourcePackState {
    SteadyTime capturedAt{};
    std::vector<ResourcePackInfo> activePacks;  // highest priority first
};

struct Aabb {
    Vec3 minimum{};
    Vec3 maximum{};
};

enum class CombatEventKind : std::uint8_t {
    LocalHitConfirmed,  // the game confirmed the local player damaged an entity
    LocalMissed,        // the game reported a local attack that hit nothing
    LocalDamaged        // the local player took damage
};

struct CombatEvent {
    CombatEventKind kind{};
    SteadyTime occurredAt{};
    std::uint64_t targetId{};  // opaque, stable per entity; 0 when not applicable
    Vec3 attackerEye{};
    Aabb targetBounds{};
};

struct SoundEvent {
    SteadyTime occurredAt{};
    std::string soundId;
    std::wstring caption;
    std::optional<Vec3> position;  // nullopt for non-positional (UI/music)
    float volume{1.0F};
};

struct ChatEvent {
    std::wstring message;
    std::wstring sender;
    std::chrono::system_clock::time_point receivedAt{};
};

}
