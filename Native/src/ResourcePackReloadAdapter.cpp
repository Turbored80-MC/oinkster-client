#include "Oinkster/ResourcePackReloadAdapter.hpp"
#include "Oinkster/Capabilities.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/SupportedBuild.hpp"

namespace Oinkster::ResourcePackReloadAdapter {

bool Install(ModuleManager& modules, std::wstring& detail) noexcept {
    detail = L"The Global Resources reload signature and native Settings owner are not uniquely validated for Minecraft "
        + std::wstring(SupportedBuild::DisplayVersion) + L"; pack Apply remains disabled.";
    try { modules.SetCapability(Capability::ResourcePackReload, {CapabilityState::Unavailable, detail}); }
    catch (...) {}
    return false;
}

bool Reload(std::span<const std::string>, std::wstring& error) noexcept {
    error = L"Live resource reload is unavailable for this Minecraft build.";
    return false;
}

bool Available() noexcept { return false; }
void Remove() noexcept {}

}
