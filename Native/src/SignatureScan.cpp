#include "Oinkster/SignatureScan.hpp"
#include <libhat/Scanner.hpp>
#include <array>

namespace Oinkster::SignatureScan {

Selection SelectUnique(std::span<const std::vector<std::uintptr_t>> matchesPerPattern) noexcept {
    Selection selection{};
    std::size_t total{};
    for (std::size_t index = 0; index < matchesPerPattern.size(); ++index) {
        for (const auto address : matchesPerPattern[index]) {
            ++total;
            selection.address = address;
            selection.patternIndex = index;
        }
    }
    if (total == 1) {
        selection.outcome = Outcome::Unique;
        return selection;
    }
    return {total == 0 ? Outcome::Missing : Outcome::Ambiguous, 0, 0};
}

Selection FindUniqueInText(std::span<const std::string_view> patterns) noexcept {
    try {
        const auto module = hat::process::get_process_module();
        const auto text = module.get_section_data(".text");
        if (text.empty()) return {Outcome::Missing, 0, 0};
        std::vector<std::vector<std::uintptr_t>> matches;
        for (const auto pattern : patterns) {
            const auto parsed = hat::parse_signature(pattern);
            if (!parsed.has_value()) return {Outcome::InvalidPattern, 0, 0};
            // Two slots: a second match is all it takes to prove ambiguity.
            std::array<hat::scan_result, 2> found{nullptr, nullptr};
            const auto [scanEnd, written] = hat::find_all_pattern(text.begin(), text.end(), found.begin(), found.end(), parsed.value());
            (void)scanEnd;
            std::vector<std::uintptr_t> addresses;
            for (auto it = found.begin(); it != written; ++it) {
                if (it->has_result()) addresses.push_back(reinterpret_cast<std::uintptr_t>(it->get()));
            }
            matches.push_back(std::move(addresses));
        }
        return SelectUnique(matches);
    } catch (...) {
        return {Outcome::Missing, 0, 0};
    }
}

std::string_view OutcomeName(Outcome outcome) noexcept {
    switch (outcome) {
    case Outcome::Unique: return "unique";
    case Outcome::Missing: return "missing";
    case Outcome::Ambiguous: return "ambiguous";
    case Outcome::InvalidPattern: return "invalid-pattern";
    }
    return "missing";
}

}
