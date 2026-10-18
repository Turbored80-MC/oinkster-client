#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Shared text formatting for HUD modules. Pure functions, unit-tested.
namespace Oinkster::Modules {

[[nodiscard]] std::string ToUtf8(std::wstring_view value);
[[nodiscard]] std::wstring FromUtf8(std::string_view value);

// "1:05" below an hour, "1:02:05" from an hour; with milliseconds "1:05.250".
[[nodiscard]] std::string FormatDuration(std::chrono::milliseconds value, bool milliseconds = false);
// Effect amplifier (0-based) as a Roman level: 0 -> "I", 3 -> "IV"; beyond
// X falls back to a decimal level so nothing is ever mis-rendered.
[[nodiscard]] std::string EffectLevel(int amplifier);
// "512 MB" / "1.25 GB"; "--" when unknown.
[[nodiscard]] std::string FormatBytes(std::optional<std::uint64_t> bytes);
// 12/24-hour wall clock from explicit fields (testable without a clock).
[[nodiscard]] std::string FormatClock(int hour, int minute, int second, bool use24Hour, bool showSeconds);
// Truncates on a UTF-8 boundary and appends "..." when shortened.
[[nodiscard]] std::string Ellipsize(std::string_view value, std::size_t maxBytes);
// Item id without its namespace, underscores as spaces: "minecraft:iron_sword" -> "iron sword".
[[nodiscard]] std::string ReadableItemId(std::string_view itemId);

}
