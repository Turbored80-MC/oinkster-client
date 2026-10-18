#pragma once

#include <cstdint>
#include <string_view>

// The single place the supported Minecraft Bedrock build is named. The
// adapter's package check, every capability reason string, and the status
// pipe all derive from these constants, so bumping the supported build is a
// one-line change that cannot leave a stale version anywhere user-visible.
//
// Changing this also requires re-validating every game-memory adapter
// (WorldTelemetry, PlayerSkinTelemetry) against the new build and updating
// Services/ReviewedClientInjector.cs's manifest expectations.
namespace Oinkster::SupportedBuild {

inline constexpr std::wstring_view PackageVersion = L"1.26.5203.0";
inline constexpr std::wstring_view DisplayVersion = L"1.26.5203";
inline constexpr std::string_view GameVersion = "1.26.50";
inline constexpr std::int32_t NetworkProtocol = 2193;
inline constexpr std::wstring_view Executable = L"Minecraft.Windows.exe";

}
