#pragma once

#include <string>

namespace Oinkster { class ModuleManager; }

namespace Oinkster {

enum class AdapterStatus { Ready, UnsupportedBuild, ValidationFailed, ReviewRequired };
struct AdapterResult { AdapterStatus status; std::wstring message; };

class IBedrockAdapter {
public:
    virtual ~IBedrockAdapter() = default;
    [[nodiscard]] virtual std::wstring SupportedVersion() const = 0;
    [[nodiscard]] virtual AdapterResult Validate() noexcept = 0;
    [[nodiscard]] virtual AdapterResult Attach(ModuleManager& modules) noexcept = 0;
    virtual void Detach() noexcept = 0;
};

class ReviewedBedrockAdapter final : public IBedrockAdapter {
public:
    [[nodiscard]] std::wstring SupportedVersion() const override;
    [[nodiscard]] AdapterResult Validate() noexcept override;
    [[nodiscard]] AdapterResult Attach(ModuleManager& modules) noexcept override;
    void Detach() noexcept override;
};

}
