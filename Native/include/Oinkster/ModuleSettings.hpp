#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Typed, self-describing module settings.
//
// Values are persisted as strings under the flat key "<module-id>.<key>"
// (settings.json "legacyModuleSettings", which the launcher also reads and
// writes). Every module declares its settings as SettingDescriptors; the
// ModuleManager normalizes every incoming value against its descriptor, so a
// malformed, out-of-range, or hand-edited value can never reach a module -
// it is clamped, or replaced by the descriptor's default.
//
// Value formats (stable; documented in Native/README.md):
//   Bool    "true" / "false"   (also accepts 1/0, yes/no, on/off)
//   Int     base-10 integer, clamped to [minimum, maximum]
//   Float   decimal, clamped to [minimum, maximum], "C" locale
//   Color   "#AARRGGBB" (also accepts "#RRGGBB" as opaque)
//   Choice  one of the descriptor's choice values
//   Key     Win32 virtual-key code 0-254 as decimal; 0 = unbound
//   Text    printable text, control characters stripped, <= 64 code units
namespace Oinkster {

enum class SettingType : std::uint8_t { Bool, Int, Float, Color, Choice, Key, Text };

struct SettingChoice {
    std::string value;
    std::string label;
};

struct SettingDescriptor {
    std::string key;
    std::string label;
    SettingType type{SettingType::Bool};
    std::string defaultValue;
    double minimum{};
    double maximum{};
    std::vector<SettingChoice> choices;
    std::string help;
    // printf-style format for slider labels, e.g. "%.0f ms".
    std::string format;
};

inline constexpr std::size_t MaxTextSettingLength = 64;

[[nodiscard]] std::optional<bool> ParseBoolSetting(std::string_view value) noexcept;
[[nodiscard]] std::optional<double> ParseNumberSetting(std::string_view value) noexcept;
[[nodiscard]] std::optional<std::uint32_t> ParseColorSetting(std::string_view value) noexcept;
[[nodiscard]] std::string FormatColorSetting(std::uint32_t argb);
[[nodiscard]] std::string FormatNumberSetting(double value, SettingType type);

// True for keys a module binding or key setting must never use: unbound
// sentinels, mouse-button virtual keys, Escape and the Windows keys. The
// configurable menu/unload keys are checked separately (they can change).
[[nodiscard]] bool IsReservedKey(std::uint32_t key) noexcept;

// Returns the canonical string for `raw` under `descriptor`: clamped when
// numeric, the default when malformed or unknown.
[[nodiscard]] std::string NormalizeSetting(const SettingDescriptor& descriptor, std::string_view raw);

class SettingsReader {
public:
    SettingsReader() = default;
    // `values` is keyed by the descriptor key (no module prefix) and must
    // already be normalized - ModuleManager guarantees that.
    SettingsReader(std::span<const SettingDescriptor> descriptors,
        std::unordered_map<std::string, std::string> values);

    [[nodiscard]] bool Bool(std::string_view key) const;
    [[nodiscard]] int Int(std::string_view key) const;
    [[nodiscard]] float Float(std::string_view key) const;
    [[nodiscard]] std::uint32_t Color(std::string_view key) const;
    [[nodiscard]] std::string Choice(std::string_view key) const;
    [[nodiscard]] std::uint32_t Key(std::string_view key) const;
    [[nodiscard]] std::string Text(std::string_view key) const;

private:
    [[nodiscard]] std::string Value(std::string_view key) const;
    std::vector<SettingDescriptor> descriptors_;
    std::unordered_map<std::string, std::string> values_;
};

}
