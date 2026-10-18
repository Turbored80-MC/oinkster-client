#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/Json.hpp"
#include "Oinkster/Json.hpp"
#include <Windows.h>
#include <libhat/Process.hpp>
#include <charconv>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <vector>

namespace Oinkster::Diagnostics {
namespace {
std::mutex logMutex;

std::filesystem::path LogDirectory() {
    wchar_t localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    return length > 0 && length < MAX_PATH
        ? std::filesystem::path(localAppData) / "Oinkster" / "Client"
        : std::filesystem::temp_directory_path() / "Oinkster";
}
}

// Milliseconds since this DLL was loaded, so a log can be read as a timeline.
const std::chrono::steady_clock::time_point loadedAt = std::chrono::steady_clock::now();

void LogLine(std::string_view level, std::string_view event, std::string_view message) noexcept {
    try {
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - loadedAt).count();
        std::scoped_lock lock(logMutex);
        const auto directory = LogDirectory();
        std::filesystem::create_directories(directory);
        std::ofstream output(directory / "oinkster-native.log", std::ios::app);
        // Escaped: messages carry reasons and paths that may contain quotes
        // or backslashes, which used to produce invalid JSON lines.
        output << "{\"ms\":" << millis << ",\"level\":\"" << Json::Escape(level) << "\",\"event\":\"" << Json::Escape(event)
               << "\",\"message\":\"" << Json::Escape(message) << "\"}\n";
    } catch (...) {}
}

void LogFuzzySignatureScan(std::string_view event, std::string_view pattern,
    int maxMismatches, std::size_t maxResults) noexcept {
    try {
        std::vector<std::optional<std::uint8_t>> bytes;
        std::size_t pos = 0;
        while (pos < pattern.size()) {
            while (pos < pattern.size() && pattern[pos] == ' ') ++pos;
            if (pos >= pattern.size()) break;
            const std::size_t start = pos;
            while (pos < pattern.size() && pattern[pos] != ' ') ++pos;
            const std::string_view token = pattern.substr(start, pos - start);
            if (token == "?") {
                bytes.emplace_back(std::nullopt);
                continue;
            }
            std::uint8_t value{};
            const auto result = std::from_chars(token.data(), token.data() + token.size(), value, 16);
            if (result.ec != std::errc{}) return;
            bytes.emplace_back(value);
        }
        if (bytes.empty()) return;

        const auto module = hat::process::get_process_module();
        const auto text = module.get_section_data(".text");
        if (text.size() < bytes.size()) return;

        std::size_t matchesFound = 0;
        for (std::size_t i = 0; i + bytes.size() <= text.size() && matchesFound < maxResults; ++i) {
            int mismatches = 0;
            for (std::size_t j = 0; j < bytes.size(); ++j) {
                if (!bytes[j].has_value()) continue;
                if (static_cast<std::uint8_t>(text[i + j]) != *bytes[j] && ++mismatches > maxMismatches) break;
            }
            if (mismatches > maxMismatches) continue;
            ++matchesFound;

            const std::size_t dumpLength = (std::min)(bytes.size() + 16, text.size() - i);
            std::string hex;
            hex.reserve(dumpLength * 3);
            char buffer[4];
            for (std::size_t k = 0; k < dumpLength; ++k) {
                std::snprintf(buffer, sizeof(buffer), "%02X ", static_cast<unsigned>(static_cast<std::uint8_t>(text[i + k])));
                hex += buffer;
            }
            const auto address = reinterpret_cast<std::uintptr_t>(text.data() + i);
            LogLine("info", event, std::format("rva={:#x} address={:#x} mismatches={} bytes={}",
                address - module.address(), address, mismatches, hex));
        }
        if (matchesFound == 0) {
            LogLine("warn", event, std::format("no fuzzy matches within {} mismatches ({} fixed bytes)",
                maxMismatches, bytes.size()));
        }
    } catch (...) {}
}

}
