#include "Oinkster/BedrockAdapter.hpp"
#include "Oinkster/BedrockIntegration.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/Dx12Overlay.hpp"
#include "Oinkster/MouseInputBlocker.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include <Windows.h>
#include <appmodel.h>
#include <filesystem>
#include <iterator>
#include <utility>

namespace Oinkster {
namespace {
constexpr std::wstring_view SupportedPackageVersion = SupportedBuild::PackageVersion;
constexpr std::wstring_view ExpectedExecutable = SupportedBuild::Executable;

}

std::wstring ReviewedBedrockAdapter::SupportedVersion() const { return std::wstring(SupportedPackageVersion); }
AdapterResult ReviewedBedrockAdapter::Validate() noexcept {
    wchar_t executablePath[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
    if (length == 0 || length == std::size(executablePath)) {
        return { AdapterStatus::ValidationFailed, L"The host executable could not be identified." };
    }

    const std::filesystem::path path(executablePath);
    if (!path.filename().wstring().ends_with(ExpectedExecutable)) {
        return { AdapterStatus::ValidationFailed, L"The DLL is not hosted by Minecraft for Windows." };
    }

    const std::wstring expectedMarker = L"_" + std::wstring(SupportedPackageVersion) + L"_x64__";
    bool packageMatches = path.wstring().contains(expectedMarker);
    if (!packageMatches) {
        UINT32 packageLength{};
        if (GetCurrentPackageFullName(&packageLength, nullptr) == ERROR_INSUFFICIENT_BUFFER) {
            std::wstring packageName(packageLength, L'\0');
            if (GetCurrentPackageFullName(&packageLength, packageName.data()) == ERROR_SUCCESS) {
                packageMatches = packageName.contains(expectedMarker);
            }
        }
    }
    if (!packageMatches) {
        // Built from SupportedPackageVersion rather than repeating the literal,
        // so bumping the supported build cannot leave a stale version in a
        // user-visible message.
        return { AdapterStatus::UnsupportedBuild,
            L"This DLL supports only Bedrock package " + std::wstring(SupportedPackageVersion) + L" x64." };
    }

    return { AdapterStatus::Ready, L"Bedrock " + std::wstring(SupportedPackageVersion) + L" x64 identified." };
}
AdapterResult ReviewedBedrockAdapter::Attach(ModuleManager& modules) noexcept {
    std::wstring error;
    if (!Dx12Overlay::Install(modules, error)) return { AdapterStatus::ValidationFailed, std::move(error) };
    std::wstring inputDetail;
    (void)MouseInputBlocker::Install(modules, inputDetail);
    (void)MouseInputBlocker::PollInstall(inputDetail);
    // Best-effort, independently capability-gated integrations. A failure in
    // one source never prevents presentation or another source from starting.
    std::wstring integrationDetail;
    (void)BedrockIntegration::Install(modules, integrationDetail);
    return { AdapterStatus::Ready, L"Direct3D 12 presentation and scoped input routing installed." };
}
void ReviewedBedrockAdapter::Detach() noexcept { BedrockIntegration::Remove(); MouseInputBlocker::Remove(); Dx12Overlay::Remove(); }

}
