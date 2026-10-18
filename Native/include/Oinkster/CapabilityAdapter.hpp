#pragma once

#include "Capabilities.hpp"
#include <span>
#include <string>
#include <string_view>

namespace Oinkster { class ModuleManager; }

namespace Oinkster {

struct CapabilityAdapterResult {
    bool installed{};
    std::wstring detail;
};

// Internal lifecycle contract for one independently validated Bedrock data
// source. Implementations own their hooks and report live capability state
// through ModuleManager; modules never receive an adapter or game pointer.
class ICapabilityAdapter {
public:
    virtual ~ICapabilityAdapter() = default;
    [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
    [[nodiscard]] virtual std::span<const Capability> Capabilities() const noexcept = 0;
    [[nodiscard]] virtual CapabilityAdapterResult Install(ModuleManager& modules) noexcept = 0;
    virtual void Remove() noexcept = 0;
};

}
