#pragma once

#include "Module.hpp"
#include <filesystem>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Oinkster {

class ModuleManager;

// settings.json, shared with the launcher (Models/ModuleDefinition.cs:
// ModuleSettingsDocument). Schema 2:
//   { "schemaVersion": 2, "revision": n, "menuKey": vk, "unloadKey": vk,
//     "modules": { "<id>": { "enabled": bool, "keybind": vk|null,
//                            "hud": { x, y, scale, background, opacity,
//                                     backgroundColor, textColor, accentColor },
//                            "options": { "<key>": "<value>" } } },
//     "legacyModuleSettings": { "<module-id>.<key>": "<value>" } }
// Schema 1 files are migrated on load. Module setting keys and value formats
// are documented in Native/README.md and ModuleSettings.hpp.
struct ClientSettings {
    static constexpr int SchemaVersion = 2;
    static constexpr std::uint32_t DefaultMenuKey = 0x4B;    // K
    static constexpr std::uint32_t DefaultUnloadKey = 0xDC;  // VK_OEM_5
    std::uint64_t revision{};
    std::uint32_t clickGuiKey{DefaultMenuKey};
    std::uint32_t unloadKey{DefaultUnloadKey};
    std::wstring theme{L"Oinkster"};
    std::unordered_map<std::string, bool> moduleStates;
    std::unordered_map<std::string, std::uint32_t> keybinds;
    std::unordered_map<std::string, HudLayout> layout;
    std::unordered_map<std::string, std::string> moduleSettings;
};

class SettingsStore final {
public:
    explicit SettingsStore(std::filesystem::path path) : path_(std::move(path)) {}
    // Never throws. A file that cannot be parsed is renamed to
    // settings.json.corrupt.<ticks> and defaults are returned; a transient
    // read failure leaves the file alone.
    [[nodiscard]] ClientSettings Load() const;
    // Throws std::runtime_error when the file cannot be written; the previous
    // file is left intact (atomic replace).
    void Save(const ClientSettings& settings) const;
private:
    std::filesystem::path path_;
};

// Named snapshots of the complete client configuration. Profiles live beside
// settings.json in their own directory so launchers that only understand the
// settings schema cannot accidentally discard them while saving.
class SettingsProfileStore final {
public:
    explicit SettingsProfileStore(std::filesystem::path directory) : directory_(std::move(directory)) {}
    [[nodiscard]] static bool IsValidName(std::string_view name) noexcept;
    [[nodiscard]] std::vector<std::string> List() const;
    [[nodiscard]] std::optional<ClientSettings> Load(std::string_view name) const;
    void Save(std::string_view name, const ClientSettings& settings) const;
    [[nodiscard]] bool Remove(std::string_view name) const noexcept;
    [[nodiscard]] std::string Active() const;
    void SetActive(std::string_view name) const;
private:
    [[nodiscard]] std::filesystem::path ProfilePath(std::string_view name) const;
    std::filesystem::path directory_;
};

// Parses a settings document already read into memory (exposed for tests).
// nullopt means "corrupt / unsupported schema".
[[nodiscard]] std::optional<ClientSettings> ParseClientSettings(std::string_view json);
[[nodiscard]] std::string SerializeClientSettings(const ClientSettings& settings);

// Pushes persisted settings into the module manager: enabled preferences
// (deferred until capabilities validate), keybinds, module settings and HUD
// layouts. Used for the initial load and for every launcher reload.
void ApplySettings(ModuleManager& modules, const ClientSettings& settings);
// Captures the module manager's current state for saving.
[[nodiscard]] ClientSettings CaptureSettings(const ModuleManager& modules, std::uint32_t menuKey,
    std::uint32_t unloadKey, std::uint64_t revision);

}
