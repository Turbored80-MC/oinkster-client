#include "Oinkster/ModuleSettings.hpp"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace Oinkster {
namespace {
std::string Lower(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return result;
}

std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

const SettingDescriptor* FindDescriptor(std::span<const SettingDescriptor> descriptors, std::string_view key) noexcept {
    const auto found = std::ranges::find(descriptors, key, &SettingDescriptor::key);
    return found == descriptors.end() ? nullptr : &*found;
}
}

std::optional<bool> ParseBoolSetting(std::string_view value) noexcept {
    const auto trimmed = Trim(value);
    if (trimmed.size() > 8) return std::nullopt;
    std::string lowered;
    for (const char character : trimmed) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on") return true;
    if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off") return false;
    return std::nullopt;
}

std::optional<double> ParseNumberSetting(std::string_view value) noexcept {
    const auto trimmed = Trim(value);
    if (trimmed.empty()) return std::nullopt;
    double parsed{};
    const auto* begin = trimmed.data();
    const auto* end = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(parsed)) return std::nullopt;
    return parsed;
}

std::optional<std::uint32_t> ParseColorSetting(std::string_view value) noexcept {
    auto trimmed = Trim(value);
    if (!trimmed.starts_with('#')) return std::nullopt;
    trimmed.remove_prefix(1);
    if (trimmed.size() != 8 && trimmed.size() != 6) return std::nullopt;
    std::uint32_t parsed{};
    const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed, 16);
    if (result.ec != std::errc{} || result.ptr != trimmed.data() + trimmed.size()) return std::nullopt;
    return trimmed.size() == 6 ? (0xFF000000U | parsed) : parsed;
}

std::string FormatColorSetting(std::uint32_t argb) { return std::format("#{:08X}", argb); }

std::optional<PixelGrid> ParsePixelGrid(std::string_view value) noexcept {
    const auto trimmed = Trim(value);
    constexpr std::size_t DigitsPerRow = 4;
    if (trimmed.size() != PixelGridSize * DigitsPerRow) return std::nullopt;
    PixelGrid grid{};
    for (std::size_t row = 0; row < PixelGridSize; ++row) {
        const auto* begin = trimmed.data() + row * DigitsPerRow;
        std::uint16_t bits{};
        const auto result = std::from_chars(begin, begin + DigitsPerRow, bits, 16);
        if (result.ec != std::errc{} || result.ptr != begin + DigitsPerRow) return std::nullopt;
        grid[row] = bits;
    }
    return grid;
}

std::string FormatPixelGrid(const PixelGrid& grid) {
    std::string text;
    text.reserve(PixelGridSize * 4);
    for (const auto row : grid) text += std::format("{:04X}", row);
    return text;
}

PixelGrid MirrorPixelsHorizontally(const PixelGrid& grid) noexcept {
    PixelGrid result = grid;
    for (std::size_t row = 0; row < PixelGridSize; ++row) {
        for (std::size_t column = 0; column < PixelGridSize / 2; ++column) {
            SetPixel(result, PixelGridSize - 1 - column, row, PixelAt(grid, column, row));
        }
    }
    return result;
}

PixelGrid MirrorPixelsVertically(const PixelGrid& grid) noexcept {
    PixelGrid result = grid;
    for (std::size_t row = 0; row < PixelGridSize / 2; ++row) result[PixelGridSize - 1 - row] = grid[row];
    return result;
}

std::string FormatNumberSetting(double value, SettingType type) {
    if (type == SettingType::Int || type == SettingType::Key) return std::format("{}", static_cast<long long>(std::llround(value)));
    // Fixed precision keeps the file stable (no 0.30000000000000004 churn)
    // and round-trips every value a slider can produce.
    auto text = std::format("{:.4f}", value);
    while (text.ends_with('0')) text.pop_back();
    if (text.ends_with('.')) text.pop_back();
    if (text == "-0") text = "0";
    return text;
}

bool IsReservedKey(std::uint32_t key) noexcept {
    if (key == 0 || key > 0xFE) return true;
    // VK_LBUTTON..VK_XBUTTON2 (1-6, VK_CANCEL=3 included) are mouse buttons;
    // binding them would toggle modules on every click.
    if (key <= 0x06) return true;
    return key == VK_ESCAPE || key == VK_LWIN || key == VK_RWIN;
}

std::string NormalizeSetting(const SettingDescriptor& descriptor, std::string_view raw) {
    switch (descriptor.type) {
    case SettingType::Bool: {
        const auto parsed = ParseBoolSetting(raw);
        if (!parsed) return NormalizeSetting(descriptor, descriptor.defaultValue == raw ? "false" : descriptor.defaultValue);
        return *parsed ? "true" : "false";
    }
    case SettingType::Int:
    case SettingType::Float: {
        auto parsed = ParseNumberSetting(raw);
        if (!parsed) parsed = ParseNumberSetting(descriptor.defaultValue);
        double value = parsed.value_or(descriptor.minimum);
        if (descriptor.maximum > descriptor.minimum) value = std::clamp(value, descriptor.minimum, descriptor.maximum);
        return FormatNumberSetting(value, descriptor.type);
    }
    case SettingType::Color: {
        auto parsed = ParseColorSetting(raw);
        if (!parsed) parsed = ParseColorSetting(descriptor.defaultValue);
        return FormatColorSetting(parsed.value_or(0xFFFFFFFFU));
    }
    case SettingType::Choice: {
        const auto lowered = Lower(Trim(raw));
        for (const auto& choice : descriptor.choices) {
            if (choice.value == lowered) return choice.value;
        }
        if (raw != descriptor.defaultValue) return NormalizeSetting(descriptor, descriptor.defaultValue);
        return descriptor.choices.empty() ? std::string{} : descriptor.choices.front().value;
    }
    case SettingType::Key: {
        const auto parsed = ParseNumberSetting(raw);
        if (!parsed || *parsed < 0.0 || *parsed > 254.0 || std::floor(*parsed) != *parsed) {
            return raw != descriptor.defaultValue ? NormalizeSetting(descriptor, descriptor.defaultValue) : "0";
        }
        const auto key = static_cast<std::uint32_t>(*parsed);
        return key != 0 && IsReservedKey(key) ? "0" : std::to_string(key);
    }
    case SettingType::Text: {
        std::string result;
        for (const char character : raw) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20 || byte == 0x7F) continue;
            result.push_back(character);
            if (result.size() >= MaxTextSettingLength) break;
        }
        // Never leave a dangling partial UTF-8 sequence after truncation:
        // find where the final sequence starts and drop it if incomplete.
        if (!result.empty()) {
            std::size_t start = result.size() - 1;
            while (start > 0 && (static_cast<unsigned char>(result[start]) & 0xC0U) == 0x80U) --start;
            const auto lead = static_cast<unsigned char>(result[start]);
            const std::size_t expected = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (result.size() - start < expected) result.resize(start);
        }
        return result;
    }
    case SettingType::Pixels: {
        auto parsed = ParsePixelGrid(raw);
        if (!parsed) parsed = ParsePixelGrid(descriptor.defaultValue);
        return FormatPixelGrid(parsed.value_or(PixelGrid{}));
    }
    }
    return descriptor.defaultValue;
}

bool IsSettingVisible(const SettingDescriptor& descriptor, const SettingsReader& reader) {
    if (descriptor.visibleWhenKey.empty()) return true;
    return std::ranges::find(descriptor.visibleWhenValues, reader.Raw(descriptor.visibleWhenKey)) != descriptor.visibleWhenValues.end();
}

SettingsReader::SettingsReader(std::span<const SettingDescriptor> descriptors,
    std::unordered_map<std::string, std::string> values)
    : descriptors_(descriptors.begin(), descriptors.end()), values_(std::move(values)) {}

std::string SettingsReader::Value(std::string_view key) const {
    const auto* descriptor = FindDescriptor(descriptors_, key);
    const auto found = values_.find(std::string(key));
    if (!descriptor) return found == values_.end() ? std::string{} : found->second;
    return NormalizeSetting(*descriptor, found == values_.end() ? std::string_view(descriptor->defaultValue)
                                                                 : std::string_view(found->second));
}

bool SettingsReader::Bool(std::string_view key) const { return Value(key) == "true"; }
int SettingsReader::Int(std::string_view key) const {
    return static_cast<int>(std::llround(ParseNumberSetting(Value(key)).value_or(0.0)));
}
float SettingsReader::Float(std::string_view key) const {
    return static_cast<float>(ParseNumberSetting(Value(key)).value_or(0.0));
}
std::uint32_t SettingsReader::Color(std::string_view key) const { return ParseColorSetting(Value(key)).value_or(0xFFFFFFFFU); }
std::string SettingsReader::Choice(std::string_view key) const { return Value(key); }
std::uint32_t SettingsReader::Key(std::string_view key) const {
    return static_cast<std::uint32_t>(ParseNumberSetting(Value(key)).value_or(0.0));
}
std::string SettingsReader::Text(std::string_view key) const { return Value(key); }
PixelGrid SettingsReader::Pixels(std::string_view key) const { return ParsePixelGrid(Value(key)).value_or(PixelGrid{}); }

}
