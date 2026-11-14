#pragma once

#include "Module.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Oinkster {

struct ModuleAvailability {
    bool available{true};
    std::wstring unavailableReason;
};

// Registration, lifecycle, dispatch, keybinds, availability and settings.
// Feature logic lives in the modules themselves (Native/src/Modules).
//
// Availability is capability-based: a module is available exactly when
// every capability in its metadata's requiredCapabilities is Available.
// Adapters report capabilities through SetCapability; modules never decide
// their own availability.
//
// Enabled state has two layers:
//   * active    - the module is enabled and receiving events;
//   * requested - the player wants it on (persisted). A requested module
//                 whose capabilities are not (yet) available waits, and is
//                 activated automatically when they validate - so a module
//                 enabled last session survives restarting before the
//                 adapter has attached.
class ModuleManager final {
public:
    using DiagnosticsSink = std::function<void(std::string_view level, std::string_view event, std::string_view message)>;

    ModuleManager();

    void Register(std::unique_ptr<IModule> module);
    void SetDiagnosticsSink(DiagnosticsSink sink);

    // Player-initiated toggle. Enabling an unavailable module is refused.
    [[nodiscard]] bool SetEnabled(std::string_view id, bool enabled) noexcept;
    // Persisted preference. Enabling an unavailable module defers it.
    void RestoreEnabled(std::string_view id, bool enabled) noexcept;
    [[nodiscard]] bool IsEnabled(std::string_view id) const noexcept;
    [[nodiscard]] bool IsRequested(std::string_view id) const noexcept;
    void DisableAll() noexcept;

    void SetCapability(Capability capability, CapabilityStatus status);
    [[nodiscard]] CapabilityStatus CapabilityStatusOf(Capability capability) const;
    [[nodiscard]] std::array<CapabilityStatus, CapabilityCount> Capabilities() const;
    // Monotonically increases whenever a capability state or reason changes.
    // The status pipe uses it to publish asynchronous adapter failures.
    [[nodiscard]] std::uint64_t CapabilityRevision() const noexcept;
    [[nodiscard]] ModuleAvailability Availability(std::string_view id) const;

    // Registration order, with live availability merged in.
    [[nodiscard]] std::vector<ModuleMetadata> Catalog() const;
    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] IModule* FindModule(std::string_view id) const noexcept;
    [[nodiscard]] IHudModule* FindHudModule(std::string_view id) const noexcept;
    [[nodiscard]] bool HasEnabledHudModules() const noexcept;
    // True when anything the overlay draws or applies per frame is enabled
    // (HUD, overlay geometry, frame blending). The Present hook skips ImGui
    // entirely when this is false.
    [[nodiscard]] bool HasEnabledOverlayModules() const noexcept;
    [[nodiscard]] std::vector<IHudModule*> EnabledHudModules() const;

    template<typename T> [[nodiscard]] std::vector<T*> EnabledModulesOf() const {
        std::vector<T*> result;
        std::shared_lock lock(mutex_);
        for (const auto& id : registrationOrder_) {
            if (!enabled_.contains(id)) continue;
            if (auto* typed = dynamic_cast<T*>(modules_.at(id).get())) result.push_back(typed);
        }
        return result;
    }

    [[nodiscard]] bool SetKeybind(std::string_view id, std::uint32_t key, std::uint32_t menuKey, std::uint32_t unloadKey) noexcept;
    [[nodiscard]] std::uint32_t Keybind(std::string_view id) const noexcept;
    // Other modules bound to the same key as `id` (duplicates are allowed -
    // one key may deliberately toggle a group - but the UI warns).
    [[nodiscard]] std::vector<std::string> KeybindConflicts(std::string_view id) const;
    // Toggles every module bound to `key`. Returns true only if at least one
    // module actually changed state, so callers can avoid persisting settings
    // for a keypress that was bound to nothing.
    [[nodiscard]] bool HandleKeybind(std::uint32_t key) noexcept;

    // Flat "<module-id>.<key>" settings. Values for a registered module's
    // declared settings are normalized (clamped / defaulted) on the way in;
    // other keys (e.g. "click-gui.scale") are stored verbatim within bounds.
    void SetModuleSetting(std::string key, std::string value);
    [[nodiscard]] std::string ModuleSetting(std::string_view key, std::string_view fallback) const;
    [[nodiscard]] std::unordered_map<std::string, std::string> ModuleSettings() const;
    // Replaces every stored setting (a launcher reload) and notifies modules.
    void ReplaceModuleSettings(const std::unordered_map<std::string, std::string>& values);
    // Removes every stored value for `id`, restoring its defaults.
    void ResetModuleSettings(std::string_view id);
    [[nodiscard]] SettingsReader SettingsFor(std::string_view id) const;

    bool InvokeAction(std::string_view id, std::string_view action, SteadyTime now);

    static constexpr std::size_t MaxStoredSettings = 4096;
    static constexpr std::size_t MaxSettingKeyLength = 128;
    static constexpr std::size_t MaxSettingValueLength = 512;

    template<typename Event> void Publish(const Event& event) noexcept { PublishInternal(event); }

    [[nodiscard]] bool PublishInput(const KeyboardEvent& event) noexcept {
        PublishInternal(event);
        return !event.IsCancelled();
    }

    [[nodiscard]] bool PublishInput(const MouseEvent& event) noexcept {
        PublishInternal(event);
        return !event.IsCancelled();
    }

private:
    template<typename Event> void PublishInternal(const Event& event) noexcept {
        std::vector<IModule*> snapshot;
        {
            std::shared_lock lock(mutex_);
            for (const auto& id : registrationOrder_) {
                if (enabled_.contains(id)) snapshot.push_back(modules_.at(id).get());
            }
        }
        std::ranges::stable_sort(snapshot, {}, [](const IModule* module) {
            return module->Metadata().eventPriority;
        });
        for (auto* module : snapshot) {
            try { Dispatch(*module, event); } catch (...) { (void)SetEnabled(module->Metadata().id, false); }
        }
    }
    static void Dispatch(IModule& module, const RenderEvent& value) { module.OnRender(value); }
    static void Dispatch(IModule& module, const TickEvent& value) { module.OnTick(value); }
    static void Dispatch(IModule& module, const KeyboardEvent& value) { module.OnKeyboard(value); }
    static void Dispatch(IModule& module, const MouseEvent& value) { module.OnMouse(value); }
    static void Dispatch(IModule& module, const PlayerTransform& value) { module.OnPlayerTransform(value); }
    static void Dispatch(IModule& module, const EquipmentSnapshot& value) { module.OnEquipment(value); }
    static void Dispatch(IModule& module, const InventorySnapshot& value) { module.OnInventory(value); }
    static void Dispatch(IModule& module, const EffectsSnapshot& value) { module.OnEffects(value); }
    static void Dispatch(IModule& module, const NetworkMetrics& value) { module.OnNetworkMetrics(value); }
    static void Dispatch(IModule& module, const ServerState& value) { module.OnServerState(value); }
    static void Dispatch(IModule& module, const ResourcePackState& value) { module.OnResourcePacks(value); }
    static void Dispatch(IModule& module, const CombatEvent& value) { module.OnCombat(value); }
    static void Dispatch(IModule& module, const SoundEvent& value) { module.OnSound(value); }
    static void Dispatch(IModule& module, const ChatEvent& value) { module.OnChat(value); }
    static void Dispatch(IModule& module, const CameraPerspectiveEvent& value) { module.OnCameraPerspective(value); }
    static void Dispatch(IModule& module, const GameCrosshairEvent& value) { module.OnGameCrosshair(value); }
    static void Dispatch(IModule& module, const BlockSelectionEvent& value) { module.OnBlockSelection(value); }
    static void Dispatch(IModule& module, const ReplayPacketEvent& value) { module.OnReplayPacket(value); }
    static void Dispatch(IModule& module, const CameraPoseEvent& value) { module.OnCameraPose(value); }

    [[nodiscard]] ModuleAvailability ComputeAvailabilityLocked(const IModule& module) const;
    [[nodiscard]] SettingsReader SettingsForLocked(const IModule& module) const;
    void NotifySettings(const std::vector<std::string>& ids);
    void Log(std::string_view level, std::string_view event, std::string_view message) const noexcept;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<IModule>> modules_;
    std::vector<std::string> registrationOrder_;
    std::unordered_set<std::string> enabled_;
    std::unordered_set<std::string> pending_;
    std::unordered_map<std::string, ModuleAvailability> availability_;
    std::array<CapabilityStatus, CapabilityCount> capabilities_{};
    std::uint64_t capabilityRevision_{};
    std::unordered_map<std::string, std::uint32_t> keybinds_;
    std::unordered_map<std::string, std::string> settings_;
    DiagnosticsSink sink_;
};

std::vector<std::unique_ptr<IModule>> CreateModuleCatalog();

}
