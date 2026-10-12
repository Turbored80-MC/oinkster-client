#pragma once

#include <cstddef>
#include <string_view>

namespace Oinkster::Diagnostics {

// Appends one JSON line to %LOCALAPPDATA%\Oinkster\Client\oinkster-native.log,
// the same file Runtime.cpp writes to. Safe to call from any thread.
void LogLine(std::string_view level, std::string_view event, std::string_view message) noexcept;

// One-shot recovery aid for a signature scan that just failed: re-scans the
// host process's .text section allowing up to `maxMismatches` of the
// pattern's fixed (non-"?") bytes to differ, and logs up to `maxResults`
// candidate locations (RVA, absolute address, mismatch count, and a hex dump
// of the matched window plus trailing context) under `event`. `pattern` uses
// the same space-separated hex/"?" syntax as hat::parse_signature. Meant to
// recover real ground-truth bytes from a live game process when a maintained
// signature table has gone stale for the exact installed build - not for
// hot-path use (a full scan is O(section size) and only runs on failure).
void LogFuzzySignatureScan(std::string_view event, std::string_view pattern,
    int maxMismatches, std::size_t maxResults) noexcept;

}
