#include "Oinkster/ModuleManager.hpp"
#include <Windows.h>
#include <chrono>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    Oinkster::ModuleManager modules;
    for (auto& module : Oinkster::CreateModuleCatalog()) modules.Register(std::move(module));
    (void)modules.SetEnabled("fps", true);
    // The harness plays the role of a validated adapter: it reports the
    // capabilities its simulated events stand in for.
    modules.SetCapability(Oinkster::Capability::LocalPlayerPosition, {Oinkster::CapabilityState::Available, {}});
    modules.SetCapability(Oinkster::Capability::NetworkMetrics, {Oinkster::CapabilityState::Available, {}});
    (void)modules.SetEnabled("coordinates", true);
    (void)modules.SetEnabled("ping", true);
    const auto now = std::chrono::steady_clock::now();
    modules.Publish(Oinkster::PlayerTransform{now, {12.5, 64.0, -31.25}, 90.0F, 0.0F, 0});
    modules.Publish(Oinkster::NetworkMetrics{now, 42U});
    const std::wstring message = L"Oinkster Module Harness\n\nLoaded " + std::to_wstring(modules.Size())
        + L" fair-play modules.\nSimulated player, network, and render events completed.";
    MessageBoxW(nullptr, message.c_str(), L"Oinkster", MB_OK | MB_ICONINFORMATION);
    modules.DisableAll();
    return modules.Size() == 36 ? 0 : 1;
}
