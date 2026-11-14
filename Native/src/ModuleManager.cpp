#include "Oinkster/ModuleManager.hpp"
#include <mutex>
#include <stdexcept>

namespace Oinkster {
namespace {
std::string Narrow(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) result.push_back(character <= 0x7f ? static_cast<char>(character) : '?');
    return result;
}

// "<module-id>.<key>" -> {"<module-id>", "<key>"}. Module ids never contain
// '.', so the first dot is the separator.
std::pair<std::string_view, std::string_view> SplitSettingKey(std::string_view key) noexcept {
    const auto dot = key.find('.');
    if (dot == std::string_view::npos) return {key, {}};
    return {key.substr(0, dot), key.substr(dot + 1)};
}

const SettingDescriptor* FindDescriptor(const IModule& module, std::string_view key) noexcept {
    for (const auto& descriptor : module.Settings()) {
        if (descriptor.key == key) return &descriptor;
    }
    return nullptr;
}
}

ModuleManager::ModuleManager() {
    for (std::size_t index = 0; index < CapabilityCount; ++index) {
        capabilities_[index] = DefaultCapabilityStatus(static_cast<Capability>(index));
    }
}

void ModuleManager::SetDiagnosticsSink(DiagnosticsSink sink) {
    std::unique_lock lock(mutex_);
    sink_ = std::move(sink);
}

void ModuleManager::Log(std::string_view level, std::string_view event, std::string_view message) const noexcept {
    DiagnosticsSink sink;
    {
        std::shared_lock lock(mutex_);
        sink = sink_;
    }
    if (!sink) return;
    try { sink(level, event, message); } catch (...) {}
}

void ModuleManager::Register(std::unique_ptr<IModule> module) {
    if (!module || module->Metadata().id.empty()) throw std::invalid_argument("Module id is required");
    const auto id = module->Metadata().id;
    if (id.find('.') != std::string::npos) throw std::invalid_argument("Module ids must not contain '.': " + id);
    for (const auto capability : module->Metadata().requiredCapabilities) {
        if (static_cast<std::size_t>(capability) >= CapabilityCount) throw std::invalid_argument("Unknown capability for " + id);
    }
    IModule* registered{};
    bool enableNow{};
    {
        std::unique_lock lock(mutex_);
        if (modules_.contains(id)) throw std::invalid_argument("Duplicate module id: " + id);
        availability_[id] = ComputeAvailabilityLocked(*module);
        if (module->Metadata().defaultEnabled) {
            if (availability_[id].available) enableNow = true;
            else pending_.insert(id);
        }
        registered = module.get();
        modules_.emplace(id, std::move(module));
        registrationOrder_.push_back(id);
    }
    try { registered->OnSettingsChanged(SettingsFor(id)); } catch (...) {}
    if (enableNow) (void)SetEnabled(id, true);
}

ModuleAvailability ModuleManager::ComputeAvailabilityLocked(const IModule& module) const {
    for (const auto capability : module.Metadata().requiredCapabilities) {
        const auto& status = capabilities_[static_cast<std::size_t>(capability)];
        if (status.IsAvailable()) continue;
        std::wstring reason = L"Requires ";
        reason += CapabilityName(capability);
        reason += L". ";
        reason += status.reason.empty() ? std::wstring(L"Not validated for this build.") : status.reason;
        return {false, std::move(reason)};
    }
    return {true, {}};
}

bool ModuleManager::SetEnabled(std::string_view id, bool enabled) noexcept {
    IModule* target{};
    {
        std::unique_lock lock(mutex_);
        const auto found = modules_.find(std::string(id));
        if (found == modules_.end()) return false;
        if (enabled && !availability_[found->first].available) return false;
        // An explicit choice replaces any deferred preference.
        pending_.erase(found->first);
        const bool current = enabled_.contains(found->first);
        if (current == enabled) return true;
        if (enabled) enabled_.insert(found->first); else enabled_.erase(found->first);
        target = found->second.get();
    }
    try { enabled ? target->OnEnable() : target->OnDisable(); }
    catch (...) { if (enabled) (void)SetEnabled(id, false); return false; }
    return true;
}

void ModuleManager::RestoreEnabled(std::string_view id, bool enabled) noexcept {
    if (!enabled) { (void)SetEnabled(id, false); return; }
    bool defer{};
    {
        std::unique_lock lock(mutex_);
        const auto found = modules_.find(std::string(id));
        if (found == modules_.end()) return;
        if (!availability_[found->first].available) {
            pending_.insert(found->first);
            defer = true;
        }
    }
    if (!defer) (void)SetEnabled(id, true);
}

bool ModuleManager::IsEnabled(std::string_view id) const noexcept {
    std::shared_lock lock(mutex_);
    return enabled_.contains(std::string(id));
}

bool ModuleManager::IsRequested(std::string_view id) const noexcept {
    std::shared_lock lock(mutex_);
    const std::string owned(id);
    return enabled_.contains(owned) || pending_.contains(owned);
}

void ModuleManager::DisableAll() noexcept {
    std::vector<IModule*> active;
    {
        std::unique_lock lock(mutex_);
        for (const auto& id : registrationOrder_) if (enabled_.contains(id)) active.push_back(modules_.at(id).get());
        enabled_.clear();
        pending_.clear();
    }
    for (auto* module : active) { try { module->OnDisable(); } catch (...) {} }
}

void ModuleManager::SetCapability(Capability capability, CapabilityStatus status) {
    const auto index = static_cast<std::size_t>(capability);
    if (index >= CapabilityCount) return;
    if (status.IsAvailable()) status.reason.clear();
    std::vector<IModule*> toDisable;
    std::vector<std::string> toEnable;
    bool changed{};
    {
        std::unique_lock lock(mutex_);
        auto& current = capabilities_[index];
        changed = current.state != status.state || current.reason != status.reason;
        current = status;
        if (changed) ++capabilityRevision_;
        for (const auto& id : registrationOrder_) {
            auto& module = *modules_.at(id);
            auto next = ComputeAvailabilityLocked(module);
            if (!next.available && enabled_.contains(id)) {
                enabled_.erase(id);
                pending_.insert(id);  // keep the preference; re-enable when it validates again
                toDisable.push_back(&module);
            } else if (next.available && pending_.contains(id)) {
                toEnable.push_back(id);
            }
            availability_[id] = std::move(next);
        }
    }
    for (auto* module : toDisable) { try { module->OnDisable(); } catch (...) {} }
    for (const auto& id : toEnable) (void)SetEnabled(id, true);
    if (changed) {
        Log(status.IsAvailable() ? "info" : "warning", "capability",
            std::string(CapabilityId(capability)) + "=" + std::string(CapabilityStateId(status.state))
            + (status.reason.empty() ? std::string{} : " (" + Narrow(status.reason) + ")"));
    }
}

CapabilityStatus ModuleManager::CapabilityStatusOf(Capability capability) const {
    const auto index = static_cast<std::size_t>(capability);
    if (index >= CapabilityCount) return {};
    std::shared_lock lock(mutex_);
    return capabilities_[index];
}

std::array<CapabilityStatus, CapabilityCount> ModuleManager::Capabilities() const {
    std::shared_lock lock(mutex_);
    return capabilities_;
}

std::uint64_t ModuleManager::CapabilityRevision() const noexcept {
    std::shared_lock lock(mutex_);
    return capabilityRevision_;
}

ModuleAvailability ModuleManager::Availability(std::string_view id) const {
    std::shared_lock lock(mutex_);
    const auto found = availability_.find(std::string(id));
    return found == availability_.end() ? ModuleAvailability{false, L"Unknown module."} : found->second;
}

std::vector<ModuleMetadata> ModuleManager::Catalog() const {
    std::shared_lock lock(mutex_);
    std::vector<ModuleMetadata> result;
    result.reserve(registrationOrder_.size());
    for (const auto& id : registrationOrder_) {
        auto metadata = modules_.at(id)->Metadata();
        const auto& availability = availability_.at(id);
        metadata.available = availability.available;
        metadata.unavailableReason = availability.unavailableReason;
        result.push_back(std::move(metadata));
    }
    return result;
}

std::size_t ModuleManager::Size() const noexcept {
    std::shared_lock lock(mutex_);
    return modules_.size();
}

IModule* ModuleManager::FindModule(std::string_view id) const noexcept {
    std::shared_lock lock(mutex_);
    const auto found = modules_.find(std::string(id));
    return found == modules_.end() ? nullptr : found->second.get();
}

IHudModule* ModuleManager::FindHudModule(std::string_view id) const noexcept {
    return dynamic_cast<IHudModule*>(FindModule(id));
}

bool ModuleManager::HasEnabledHudModules() const noexcept {
    std::shared_lock lock(mutex_);
    for (const auto& id : enabled_) {
        if (dynamic_cast<IHudModule*>(modules_.at(id).get())) return true;
    }
    return false;
}

bool ModuleManager::HasEnabledOverlayModules() const noexcept {
    std::shared_lock lock(mutex_);
    for (const auto& id : enabled_) {
        auto* module = modules_.at(id).get();
        if (dynamic_cast<IHudModule*>(module) || dynamic_cast<IOverlayModule*>(module)
            || dynamic_cast<IDebugOverlayModule*>(module)
            || dynamic_cast<IFrameBlendModule*>(module) || dynamic_cast<IPostProcessModule*>(module)) return true;
    }
    return false;
}

std::vector<IHudModule*> ModuleManager::EnabledHudModules() const { return EnabledModulesOf<IHudModule>(); }

bool ModuleManager::SetKeybind(std::string_view id, std::uint32_t key, std::uint32_t menuKey, std::uint32_t unloadKey) noexcept {
    std::unique_lock lock(mutex_);
    const std::string owned(id);
    if (!modules_.contains(owned)) return false;
    if (key == 0) { keybinds_.erase(owned); return true; }
    if (key == menuKey || key == unloadKey || IsReservedKey(key)) return false;
    keybinds_[owned] = key;
    return true;
}

std::uint32_t ModuleManager::Keybind(std::string_view id) const noexcept {
    std::shared_lock lock(mutex_);
    const auto found = keybinds_.find(std::string(id));
    return found == keybinds_.end() ? 0U : found->second;
}

std::vector<std::string> ModuleManager::KeybindConflicts(std::string_view id) const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> result;
    const auto own = keybinds_.find(std::string(id));
    if (own == keybinds_.end()) return result;
    for (const auto& other : registrationOrder_) {
        if (other == own->first) continue;
        const auto found = keybinds_.find(other);
        if (found != keybinds_.end() && found->second == own->second) result.push_back(other);
    }
    return result;
}

bool ModuleManager::HandleKeybind(std::uint32_t key) noexcept {
    std::vector<std::string> targets;
    {
        std::shared_lock lock(mutex_);
        for (const auto& id : registrationOrder_) {
            const auto found = keybinds_.find(id);
            if (found != keybinds_.end() && found->second == key) targets.push_back(id);
        }
    }
    bool toggled = false;
    for (const auto& id : targets) {
        const bool before = IsEnabled(id);
        if (SetEnabled(id, !before) && IsEnabled(id) != before) toggled = true;
    }
    return toggled;
}

SettingsReader ModuleManager::SettingsForLocked(const IModule& module) const {
    std::unordered_map<std::string, std::string> values;
    const auto prefix = module.Metadata().id + ".";
    for (const auto& descriptor : module.Settings()) {
        const auto found = settings_.find(prefix + descriptor.key);
        if (found != settings_.end()) values.emplace(descriptor.key, found->second);
    }
    return SettingsReader(module.Settings(), std::move(values));
}

SettingsReader ModuleManager::SettingsFor(std::string_view id) const {
    std::shared_lock lock(mutex_);
    const auto found = modules_.find(std::string(id));
    return found == modules_.end() ? SettingsReader{} : SettingsForLocked(*found->second);
}

void ModuleManager::NotifySettings(const std::vector<std::string>& ids) {
    for (const auto& id : ids) {
        IModule* module{};
        SettingsReader reader;
        {
            std::shared_lock lock(mutex_);
            const auto found = modules_.find(id);
            if (found == modules_.end()) continue;
            module = found->second.get();
            reader = SettingsForLocked(*module);
        }
        try { module->OnSettingsChanged(reader); } catch (...) {}
    }
}

void ModuleManager::SetModuleSetting(std::string key, std::string value) {
    if (key.empty() || key.size() > MaxSettingKeyLength) return;
    if (value.size() > MaxSettingValueLength) value.resize(MaxSettingValueLength);
    std::string owner;
    {
        std::unique_lock lock(mutex_);
        const auto [id, settingKey] = SplitSettingKey(key);
        if (const auto found = modules_.find(std::string(id)); found != modules_.end()) {
            if (const auto* descriptor = FindDescriptor(*found->second, settingKey)) {
                value = NormalizeSetting(*descriptor, value);
                owner = found->first;
            }
        }
        if (!settings_.contains(key) && settings_.size() >= MaxStoredSettings) return;
        settings_[std::move(key)] = std::move(value);
    }
    if (!owner.empty()) NotifySettings({owner});
}

std::string ModuleManager::ModuleSetting(std::string_view key, std::string_view fallback) const {
    std::shared_lock lock(mutex_);
    const auto found = settings_.find(std::string(key));
    return found == settings_.end() ? std::string(fallback) : found->second;
}

std::unordered_map<std::string, std::string> ModuleManager::ModuleSettings() const {
    std::shared_lock lock(mutex_);
    return settings_;
}

void ModuleManager::ReplaceModuleSettings(const std::unordered_map<std::string, std::string>& values) {
    {
        std::unique_lock lock(mutex_);
        settings_.clear();
    }
    for (const auto& [key, value] : values) {
        // SetModuleSetting notifies per key; batch instead by writing first
        // and notifying every module once below.
        if (key.empty() || key.size() > MaxSettingKeyLength) continue;
        std::string normalized = value.size() > MaxSettingValueLength ? value.substr(0, MaxSettingValueLength) : value;
        std::unique_lock lock(mutex_);
        const auto [id, settingKey] = SplitSettingKey(key);
        if (const auto found = modules_.find(std::string(id)); found != modules_.end()) {
            if (const auto* descriptor = FindDescriptor(*found->second, settingKey)) normalized = NormalizeSetting(*descriptor, normalized);
        }
        if (settings_.size() >= MaxStoredSettings) break;
        settings_[key] = std::move(normalized);
    }
    std::vector<std::string> ids;
    {
        std::shared_lock lock(mutex_);
        ids = registrationOrder_;
    }
    NotifySettings(ids);
}

void ModuleManager::ResetModuleSettings(std::string_view id) {
    {
        std::unique_lock lock(mutex_);
        if (!modules_.contains(std::string(id))) return;
        const auto prefix = std::string(id) + ".";
        std::erase_if(settings_, [&](const auto& entry) { return entry.first.starts_with(prefix); });
    }
    NotifySettings({std::string(id)});
}

bool ModuleManager::InvokeAction(std::string_view id, std::string_view action, SteadyTime now) {
    IModule* module{};
    {
        std::shared_lock lock(mutex_);
        const auto found = modules_.find(std::string(id));
        if (found == modules_.end() || !availability_.at(found->first).available) return false;
        module = found->second.get();
    }
    try { return module->InvokeAction(action, now); } catch (...) { return false; }
}

}
