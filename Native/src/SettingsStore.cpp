#include "Oinkster/SettingsStore.hpp"
#include "Oinkster/Json.hpp"
#include "Oinkster/ModuleManager.hpp"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Oinkster {
namespace {
bool IsValidModuleId(std::string_view id) noexcept {
    return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-';
    });
}

bool IsValidControlKey(double value) noexcept {
    return value >= 1.0 && value <= 254.0 && std::floor(value) == value;
}

std::optional<std::uint32_t> KeyValue(const Json::Value* value) {
    if (!value) return std::nullopt;
    const auto* number = value->AsNumber();
    if (!number || !IsValidControlKey(*number)) return std::nullopt;
    return static_cast<std::uint32_t>(*number);
}

float NumberOr(const Json::Value* object, std::string_view key, float fallback) {
    const auto* value = object ? object->Find(key) : nullptr;
    const auto* number = value ? value->AsNumber() : nullptr;
    return number && std::isfinite(*number) ? static_cast<float>(*number) : fallback;
}

std::uint32_t ColorOr(const Json::Value* object, std::string_view key, std::uint32_t fallback) {
    const auto* value = object ? object->Find(key) : nullptr;
    const auto* text = value ? value->AsString() : nullptr;
    return text ? ParseColorSetting(*text).value_or(fallback) : fallback;
}

HudLayout ParseHud(const Json::Value* hud) {
    HudLayout layout{};
    if (!hud || !hud->AsObject()) return layout;
    layout.normalizedX = NumberOr(hud, "x", layout.normalizedX);
    layout.normalizedY = NumberOr(hud, "y", layout.normalizedY);
    layout.scale = NumberOr(hud, "scale", layout.scale);
    if (const auto* background = hud->Find("background"); background && background->AsBool()) layout.background = *background->AsBool();
    layout.backgroundOpacity = NumberOr(hud, "opacity", layout.backgroundOpacity);
    layout.backgroundColor = ColorOr(hud, "backgroundColor", layout.backgroundColor);
    layout.textColor = ColorOr(hud, "textColor", layout.textColor);
    layout.accentColor = ColorOr(hud, "accentColor", layout.accentColor);
    return ClampHudLayout(layout);
}

// Version-one layouts were "x,y,scale[,background[,opacity]]" strings.
std::optional<HudLayout> ParseLegacyLayout(std::string_view encoded) {
    HudLayout layout{};
    std::vector<std::string> parts;
    std::string current;
    for (const char character : encoded) {
        if (character == ',') { parts.push_back(current); current.clear(); }
        else current.push_back(character);
    }
    parts.push_back(current);
    if (parts.size() < 2) return std::nullopt;
    const auto number = [&](std::size_t index, float fallback) {
        if (index >= parts.size()) return fallback;
        const auto parsed = ParseNumberSetting(parts[index]);
        return parsed ? static_cast<float>(*parsed) : fallback;
    };
    layout.normalizedX = number(0, layout.normalizedX);
    layout.normalizedY = number(1, layout.normalizedY);
    layout.scale = number(2, layout.scale);
    if (parts.size() > 3) layout.background = parts[3] != "0";
    layout.backgroundOpacity = number(4, layout.backgroundOpacity);
    return ClampHudLayout(layout);
}

void ReadStringMap(const Json::Value* object, std::string_view stripPrefix,
    std::unordered_map<std::string, std::string>& output) {
    if (!object || !object->AsObject()) return;
    for (const auto& [key, value] : *object->AsObject()) {
        const auto* text = value.AsString();
        if (!text || key.empty() || key.size() > ModuleManager::MaxSettingKeyLength) continue;
        std::string name = key.starts_with(stripPrefix) ? key.substr(stripPrefix.size()) : key;
        if (output.size() >= ModuleManager::MaxStoredSettings) break;
        output[std::move(name)] = text->size() > ModuleManager::MaxSettingValueLength
            ? text->substr(0, ModuleManager::MaxSettingValueLength) : *text;
    }
}

void ReadControlKeys(const Json::Value& root, ClientSettings& settings) {
    if (auto key = KeyValue(root.Find("menuKey"))) settings.clickGuiKey = *key;
    else if (auto legacy = KeyValue(root.Find("clickGuiKey"))) settings.clickGuiKey = *legacy;
    if (auto key = KeyValue(root.Find("unloadKey"))) settings.unloadKey = *key;
    if (settings.clickGuiKey == settings.unloadKey) {
        settings.clickGuiKey = ClientSettings::DefaultMenuKey;
        settings.unloadKey = ClientSettings::DefaultUnloadKey;
    }
}
}

std::optional<ClientSettings> ParseClientSettings(std::string_view json) {
    const auto parsed = Json::Parse(json);
    if (!parsed || !parsed->AsObject()) return std::nullopt;
    const auto& root = *parsed;
    const auto* schema = root.Find("schemaVersion");
    const auto* version = schema ? schema->AsNumber() : nullptr;
    if (!version || (*version != 1.0 && *version != 2.0)) return std::nullopt;

    ClientSettings settings;
    ReadControlKeys(root, settings);
    if (const auto* revision = root.Find("revision"); revision && revision->AsNumber() && *revision->AsNumber() >= 0.0) {
        settings.revision = static_cast<std::uint64_t>(std::min(*revision->AsNumber(), 9.0e15));
    }

    if (*version == 2.0) {
        if (const auto* modules = root.Find("modules"); modules && modules->AsObject()) {
            for (const auto& [id, entry] : *modules->AsObject()) {
                if (!IsValidModuleId(id) || !entry.AsObject()) continue;
                if (const auto* enabled = entry.Find("enabled"); enabled && enabled->AsBool()) settings.moduleStates[id] = *enabled->AsBool();
                if (auto key = KeyValue(entry.Find("keybind"))) settings.keybinds[id] = *key;
                if (const auto* hud = entry.Find("hud"); hud && hud->AsObject()) settings.layout[id] = ParseHud(hud);
                std::unordered_map<std::string, std::string> options;
                ReadStringMap(entry.Find("options"), {}, options);
                for (auto& [key, value] : options) settings.moduleSettings[id + "." + key] = std::move(value);
            }
        }
        // The flat map wins over per-module "options" (it is what both the
        // launcher and this store actually write).
        ReadStringMap(root.Find("legacyModuleSettings"), {}, settings.moduleSettings);
    } else {
        if (const auto* modules = root.Find("modules"); modules && modules->AsObject()) {
            for (const auto& [id, enabled] : *modules->AsObject()) {
                if (IsValidModuleId(id) && enabled.AsBool()) settings.moduleStates[id] = *enabled.AsBool();
            }
        }
        if (const auto* keybinds = root.Find("keybinds"); keybinds && keybinds->AsObject()) {
            for (const auto& [key, value] : *keybinds->AsObject()) {
                const std::string id = key.starts_with("bind.") ? key.substr(5) : key;
                if (auto bound = KeyValue(&value); bound && IsValidModuleId(id)) settings.keybinds[id] = *bound;
            }
        }
        if (const auto* layouts = root.Find("layout"); layouts && layouts->AsObject()) {
            for (const auto& [key, value] : *layouts->AsObject()) {
                const std::string id = key.starts_with("layout.") ? key.substr(7) : key;
                const auto* text = value.AsString();
                if (!text || !IsValidModuleId(id)) continue;
                if (auto layout = ParseLegacyLayout(*text)) settings.layout[id] = *layout;
            }
        }
        ReadStringMap(root.Find("moduleSettings"), "setting.", settings.moduleSettings);
    }
    return settings;
}

std::string SerializeClientSettings(const ClientSettings& settings) {
    std::string output = std::format("{{\n  \"schemaVersion\": 2,\n  \"revision\": {},\n  \"menuKey\": {},\n  \"unloadKey\": {},\n  \"modules\": {{\n",
        settings.revision + 1, settings.clickGuiKey, settings.unloadKey);
    std::set<std::string> ids;
    for (const auto& [id, _] : settings.moduleStates) ids.insert(id);
    for (const auto& [id, _] : settings.keybinds) ids.insert(id);
    for (const auto& [id, _] : settings.layout) ids.insert(id);
    bool first = true;
    for (const auto& id : ids) {
        if (!IsValidModuleId(id)) continue;
        const auto state = settings.moduleStates.find(id);
        const auto bind = settings.keybinds.find(id);
        const auto found = settings.layout.find(id);
        const HudLayout layout = found == settings.layout.end() ? HudLayout{} : ClampHudLayout(found->second);
        if (!first) output += ",\n";
        first = false;
        output += std::format(
            "    \"{}\": {{ \"enabled\": {}, \"keybind\": {}, \"hud\": {{ \"x\": {:.6f}, \"y\": {:.6f}, \"scale\": {:.3f}, "
            "\"background\": {}, \"opacity\": {:.3f}, \"backgroundColor\": \"{}\", \"textColor\": \"{}\", \"accentColor\": \"{}\" }}, "
            "\"options\": {{}} }}",
            Json::Escape(id), state != settings.moduleStates.end() && state->second ? "true" : "false",
            bind == settings.keybinds.end() ? std::string("null") : std::to_string(bind->second),
            layout.normalizedX, layout.normalizedY, layout.scale, layout.background ? "true" : "false", layout.backgroundOpacity,
            FormatColorSetting(layout.backgroundColor), FormatColorSetting(layout.textColor), FormatColorSetting(layout.accentColor));
    }
    output += "\n  },\n  \"legacyModuleSettings\": {\n";
    // Sorted so the file is stable between saves.
    const std::map<std::string, std::string> sorted(settings.moduleSettings.begin(), settings.moduleSettings.end());
    first = true;
    for (const auto& [key, value] : sorted) {
        if (!first) output += ",\n";
        first = false;
        output += "    \"" + Json::Escape(key) + "\": \"" + Json::Escape(value) + "\"";
    }
    output += "\n  }\n}\n";
    return output;
}

ClientSettings SettingsStore::Load() const {
    std::error_code existsError;
    if (!std::filesystem::exists(path_, existsError)) return {};

    // Read first, and treat a read failure as "leave the file alone and carry
    // on with defaults". Only a successfully-read file that then fails to
    // PARSE is quarantined: transient I/O errors must never rename the
    // user's good settings to .corrupt.<n>.
    std::string json;
    try {
        std::ifstream input(path_, std::ios::binary);
        if (!input) return {};
        std::stringstream buffer;
        buffer << input.rdbuf();
        json = buffer.str();
        if (input.bad()) return {};
    } catch (...) {
        return {};
    }

    if (auto parsed = ParseClientSettings(json)) return std::move(*parsed);
    try {
        auto corrupt = path_;
        corrupt += L".corrupt." + std::to_wstring(std::chrono::system_clock::now().time_since_epoch().count());
        std::error_code error;
        std::filesystem::rename(path_, corrupt, error);
    } catch (...) {}
    return {};
}

void SettingsStore::Save(const ClientSettings& settings) const {
    std::filesystem::create_directories(path_.parent_path());
    auto temporary = path_;
    temporary += L".partial";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        const auto text = SerializeClientSettings(settings);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output.good()) throw std::runtime_error("Could not write settings");
    }
    // Atomic replace: the destination is never missing, and is untouched on
    // failure.
    if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("Could not replace settings (Win32 " + std::to_string(code) + ")");
    }
}

void ApplySettings(ModuleManager& modules, const ClientSettings& settings) {
    modules.ReplaceModuleSettings(settings.moduleSettings);
    for (const auto& metadata : modules.Catalog()) {
        const auto state = settings.moduleStates.find(metadata.id);
        modules.RestoreEnabled(metadata.id, state != settings.moduleStates.end() && state->second);
        (void)modules.SetKeybind(metadata.id, 0, settings.clickGuiKey, settings.unloadKey);
        if (const auto bind = settings.keybinds.find(metadata.id); bind != settings.keybinds.end()) {
            (void)modules.SetKeybind(metadata.id, bind->second, settings.clickGuiKey, settings.unloadKey);
        }
        if (auto* hud = modules.FindHudModule(metadata.id)) {
            const auto layout = settings.layout.find(metadata.id);
            hud->SetLayout(layout == settings.layout.end() ? hud->DefaultLayout() : layout->second);
        }
    }
}

ClientSettings CaptureSettings(const ModuleManager& modules, std::uint32_t menuKey, std::uint32_t unloadKey, std::uint64_t revision) {
    ClientSettings settings;
    settings.revision = revision;
    settings.clickGuiKey = menuKey;
    settings.unloadKey = unloadKey;
    settings.moduleSettings = modules.ModuleSettings();
    for (const auto& metadata : modules.Catalog()) {
        // The persisted preference, not just the active state: a module
        // waiting on a capability must not be saved as "off".
        settings.moduleStates[metadata.id] = modules.IsRequested(metadata.id);
        if (const auto key = modules.Keybind(metadata.id); key != 0) settings.keybinds[metadata.id] = key;
        if (const auto* hud = modules.FindHudModule(metadata.id)) settings.layout[metadata.id] = hud->Layout();
    }
    return settings;
}

}
