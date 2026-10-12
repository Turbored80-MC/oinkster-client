#include "Oinkster/Modules/HudFormat.hpp"
#include <Windows.h>
#include <array>
#include <format>

namespace Oinkster::Modules {

std::string ToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(std::string_view value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string FormatDuration(std::chrono::milliseconds value, bool milliseconds) {
    if (value.count() < 0) value = std::chrono::milliseconds::zero();
    const auto total = value.count();
    const auto hours = total / 3'600'000;
    const auto minutes = (total / 60'000) % 60;
    const auto seconds = (total / 1000) % 60;
    std::string text = hours > 0 ? std::format("{}:{:02}:{:02}", hours, minutes, seconds)
                                 : std::format("{}:{:02}", minutes, seconds);
    if (milliseconds) text += std::format(".{:03}", total % 1000);
    return text;
}

std::string EffectLevel(int amplifier) {
    constexpr std::array<const char*, 10> Roman{"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
    if (amplifier >= 0 && amplifier < static_cast<int>(Roman.size())) return Roman[static_cast<std::size_t>(amplifier)];
    return std::to_string(static_cast<long long>(amplifier) + 1);
}

std::string FormatBytes(std::optional<std::uint64_t> bytes) {
    if (!bytes) return "--";
    const double megabytes = static_cast<double>(*bytes) / (1024.0 * 1024.0);
    if (megabytes >= 1024.0) return std::format("{:.2f} GB", megabytes / 1024.0);
    return std::format("{:.0f} MB", megabytes);
}

std::string FormatClock(int hour, int minute, int second, bool use24Hour, bool showSeconds) {
    hour = ((hour % 24) + 24) % 24;
    minute = ((minute % 60) + 60) % 60;
    second = ((second % 60) + 60) % 60;
    if (use24Hour) {
        return showSeconds ? std::format("{:02}:{:02}:{:02}", hour, minute, second) : std::format("{:02}:{:02}", hour, minute);
    }
    const int displayHour = hour % 12 == 0 ? 12 : hour % 12;
    const char* suffix = hour < 12 ? "AM" : "PM";
    return showSeconds ? std::format("{}:{:02}:{:02} {}", displayHour, minute, second, suffix)
                       : std::format("{}:{:02} {}", displayHour, minute, suffix);
}

std::string Ellipsize(std::string_view value, std::size_t maxBytes) {
    if (value.size() <= maxBytes) return std::string(value);
    std::size_t cut = maxBytes >= 3 ? maxBytes - 3 : 0;
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0U) == 0x80U) --cut;
    return std::string(value.substr(0, cut)) + "...";
}

std::string ReadableItemId(std::string_view itemId) {
    const auto colon = itemId.find(':');
    if (colon != std::string_view::npos) itemId.remove_prefix(colon + 1);
    std::string result(itemId);
    for (auto& character : result) if (character == '_') character = ' ';
    return result;
}

}
