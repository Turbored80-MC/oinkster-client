#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Signature scanning with the project's fail-closed rule: a target is
// accepted only when exactly one location, across every candidate pattern
// for that target, matches. Zero matches is a compatibility miss; two or
// more (from one pattern or several) is ambiguous and also rejected - a
// pattern that matches twice does not identify a function.
namespace Oinkster::SignatureScan {

enum class Outcome : std::uint8_t { Unique, Missing, Ambiguous, InvalidPattern };

struct Selection {
    Outcome outcome{Outcome::Missing};
    std::uintptr_t address{};
    std::size_t patternIndex{};   // which candidate produced the unique match
};

// Pure selection over per-pattern match lists (testable without a process).
// Duplicate addresses reported by two patterns still count as ambiguous:
// overlapping patterns are a review error, not confirmation.
[[nodiscard]] Selection SelectUnique(std::span<const std::vector<std::uintptr_t>> matchesPerPattern) noexcept;

// Scans the host module's .text for every pattern (libhat syntax), keeping
// at most two matches per pattern (enough to prove ambiguity), then applies
// SelectUnique. Scanning is O(section size): call once at attach, never per
// frame.
[[nodiscard]] Selection FindUniqueInText(std::span<const std::string_view> patterns) noexcept;

[[nodiscard]] std::string_view OutcomeName(Outcome outcome) noexcept;

}
