#pragma once

#include "Module.hpp"
#include "Modules/ModuleBase.hpp"
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Oinkster {

// The native half of the module catalog contract. Assets/module-catalog.json
// is the launcher half; Oinkster.Native.Tests parses that file and fails if
// any id, name, description, category, HUD flag, capability list, static
// availability or unavailable reason differs from this table - the two can
// no longer drift silently.
struct CatalogEntry {
    std::string_view id;
    std::wstring_view name;
    std::wstring_view description;
    ModuleCategory category;
    bool hud;
    std::vector<Capability> capabilities;
};

[[nodiscard]] std::span<const CatalogEntry> CatalogEntries();
[[nodiscard]] std::string_view CategoryLabel(ModuleCategory category) noexcept;

// Build support as the launcher sees it before any adapter runs: available
// exactly when the shipped build contains an implementation for every
// required capability. Live runtime state remains capability-gated.
[[nodiscard]] bool IsStaticallyAvailable(const CatalogEntry& entry) noexcept;
// The first failing capability's default reason, or empty when available.
[[nodiscard]] std::wstring StaticUnavailableReason(const CatalogEntry& entry);

// CreateModuleCatalog() (declared in ModuleManager.hpp) with injected
// services - tests use this to supply a fixed wall clock / memory query.
[[nodiscard]] std::vector<std::unique_ptr<IModule>> CreateModuleCatalog(Modules::SharedServices& services);

}
