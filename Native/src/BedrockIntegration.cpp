#include "Oinkster/BedrockIntegration.hpp"
#include "Oinkster/CapabilityAdapter.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/PlayerSkinTelemetry.hpp"
#include "Oinkster/SessionTelemetry.hpp"
#include "Oinkster/WorldTelemetry.hpp"
#include <array>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace Oinkster::BedrockIntegration {
namespace {
bool installed{};
std::vector<std::unique_ptr<ICapabilityAdapter>> adapters;

class WorldStateAdapter final : public ICapabilityAdapter {
public:
    [[nodiscard]] std::string_view Name() const noexcept override { return "world-state"; }
    [[nodiscard]] std::span<const Capability> Capabilities() const noexcept override { return capabilities_; }
    [[nodiscard]] CapabilityAdapterResult Install(ModuleManager& modules) noexcept override {
        std::wstring detail;
        return {WorldTelemetry::Install(modules, detail), std::move(detail)};
    }
    void Remove() noexcept override { WorldTelemetry::Remove(); }

private:
    static constexpr std::array capabilities_{Capability::LocalPlayerPosition};
};

class SessionStateAdapter final : public ICapabilityAdapter {
public:
    [[nodiscard]] std::string_view Name() const noexcept override { return "session-state"; }
    [[nodiscard]] std::span<const Capability> Capabilities() const noexcept override { return capabilities_; }
    [[nodiscard]] CapabilityAdapterResult Install(ModuleManager& modules) noexcept override {
        std::wstring detail;
        return {SessionTelemetry::Install(modules, detail), std::move(detail)};
    }
    void Remove() noexcept override { SessionTelemetry::Remove(); }

private:
    static constexpr std::array capabilities_{Capability::NetworkMetrics, Capability::ServerState};
};

std::string NarrowAscii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) result.push_back(character <= 0x7f ? static_cast<char>(character) : '?');
    return result;
}
}

bool Install(ModuleManager& modules, std::wstring& detail) noexcept {
    if (installed) {
        detail = L"Bedrock integration is already installed.";
        return true;
    }

    std::wstring skinDetail;
    const bool skinInstalled = PlayerSkinTelemetry::Install(skinDetail);
    Diagnostics::LogLine(skinInstalled ? "info" : "warn", "player_skin_install", NarrowAscii(skinDetail));

    try {
        // Session state first: removal runs in reverse, so WorldTelemetry (which
        // calls SessionTelemetry::Observe) is drained before it.
        adapters.push_back(std::make_unique<SessionStateAdapter>());
        adapters.push_back(std::make_unique<WorldStateAdapter>());
        for (auto& adapter : adapters) {
            const auto result = adapter->Install(modules);
            Diagnostics::LogLine(result.installed ? "info" : "warn",
                std::string(adapter->Name()) + "_install", NarrowAscii(result.detail));
        }
    } catch (...) {
        // Allocation or an unexpected adapter exception cannot escape the DLL
        // entry path. Sources already installed remain owned for teardown.
        Diagnostics::LogLine("error", "bedrock_integration_install",
            "adapter registration failed; installed integrations remain capability-gated");
    }

    // The coordinator itself is considered installed even when an optional
    // source fails. Capability state, rather than a global boolean, is the
    // authoritative availability contract.
    installed = true;
    detail = L"Reviewed Bedrock integrations initialized.";
    return true;
}

void Remove() noexcept {
    if (!installed) return;
    // Reverse installation order.
    for (auto adapter = adapters.rbegin(); adapter != adapters.rend(); ++adapter) (*adapter)->Remove();
    adapters.clear();
    PlayerSkinTelemetry::Remove();
    installed = false;
}

}
