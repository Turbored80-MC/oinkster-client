#include "Oinkster/BedrockIntegration.hpp"
#include "Oinkster/CapabilityAdapter.hpp"
#include "Oinkster/ChatTelemetry.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/PlayerSkinTelemetry.hpp"
#include "Oinkster/PresentationAdapter.hpp"
#include "Oinkster/PacketSender.hpp"
#include "Oinkster/ReplayPacketBridge.hpp"
#include "Oinkster/ReplayInjector.hpp"
#include "Oinkster/ReplayWorld.hpp"
#include "Oinkster/ScreenTelemetry.hpp"
#include "Oinkster/SessionTelemetry.hpp"
#include "Oinkster/SoundTelemetry.hpp"
#include "Oinkster/VideoRecorder.hpp"
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

class SoundAdapter final : public ICapabilityAdapter {
public:
    [[nodiscard]] std::string_view Name() const noexcept override { return "sound-events"; }
    [[nodiscard]] std::span<const Capability> Capabilities() const noexcept override { return capabilities_; }
    [[nodiscard]] CapabilityAdapterResult Install(ModuleManager& modules) noexcept override {
        std::wstring detail;
        return {SoundTelemetry::Install(modules, detail), std::move(detail)};
    }
    void Remove() noexcept override { SoundTelemetry::Remove(); }

private:
    static constexpr std::array capabilities_{Capability::SoundEvents};
};

class ChatAdapter final : public ICapabilityAdapter {
public:
    [[nodiscard]] std::string_view Name() const noexcept override { return "chat-events"; }
    [[nodiscard]] std::span<const Capability> Capabilities() const noexcept override { return capabilities_; }
    [[nodiscard]] CapabilityAdapterResult Install(ModuleManager& modules) noexcept override {
        std::wstring detail;
        return {ChatTelemetry::Install(modules, detail), std::move(detail)};
    }
    void Remove() noexcept override { ChatTelemetry::Remove(); }

private:
    static constexpr std::array capabilities_{Capability::ChatEvents};
};

class PresentationRenderAdapter final : public ICapabilityAdapter {
public:
    [[nodiscard]] std::string_view Name() const noexcept override { return "presentation"; }
    [[nodiscard]] std::span<const Capability> Capabilities() const noexcept override { return capabilities_; }
    [[nodiscard]] CapabilityAdapterResult Install(ModuleManager& modules) noexcept override {
        std::wstring detail;
        return {PresentationAdapter::Install(modules, detail), std::move(detail)};
    }
    void Remove() noexcept override { PresentationAdapter::Remove(); }

private:
    static constexpr std::array capabilities_{Capability::CameraPresentation, Capability::CameraRotation,
        Capability::CameraMotion, Capability::ThirdPersonPresentation, Capability::LightingPresentation,
        Capability::TimePresentation, Capability::DamageRender, Capability::WeatherPresentation,
        Capability::ScreenEffectsPresentation, Capability::HandPresentation, Capability::SelectedBlockRender,
        Capability::PerspectivePresentation};
};

class PacketSendAdapter final : public ICapabilityAdapter {
public:
    [[nodiscard]] std::string_view Name() const noexcept override { return "packet-send"; }
    [[nodiscard]] std::span<const Capability> Capabilities() const noexcept override { return capabilities_; }
    [[nodiscard]] CapabilityAdapterResult Install(ModuleManager& modules) noexcept override {
        std::wstring detail;
        return {PacketSender::Install(modules, detail), std::move(detail)};
    }
    void Remove() noexcept override { PacketSender::Remove(); }

private:
    static constexpr std::array capabilities_{Capability::PacketSend};
};

class PacketCaptureAdapter final : public ICapabilityAdapter {
public:
    [[nodiscard]] std::string_view Name() const noexcept override { return "packet-capture"; }
    [[nodiscard]] std::span<const Capability> Capabilities() const noexcept override { return capabilities_; }
    [[nodiscard]] CapabilityAdapterResult Install(ModuleManager& modules) noexcept override {
        std::wstring detail;
        return {ReplayPacketBridge::Install(modules, detail), std::move(detail)};
    }
    void Remove() noexcept override { ReplayPacketBridge::Remove(); }

private:
    static constexpr std::array capabilities_{Capability::PacketCapture};
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

    std::wstring screenDetail;
    const bool screenInstalled = ScreenTelemetry::Install(screenDetail);
    Diagnostics::LogLine(screenInstalled ? "info" : "warn", "screen_telemetry_install", NarrowAscii(screenDetail));

    try {
        // Session state first: removal runs in reverse, so WorldTelemetry (which
        // calls SessionTelemetry::Observe) is drained before it.
        adapters.push_back(std::make_unique<SessionStateAdapter>());
        adapters.push_back(std::make_unique<WorldStateAdapter>());
        adapters.push_back(std::make_unique<SoundAdapter>());
        adapters.push_back(std::make_unique<ChatAdapter>());
        // The sender before the capture: the capture's receive hook drives
        // the sender's Poll, so it must be removed (reverse order) first.
        adapters.push_back(std::make_unique<PacketSendAdapter>());
        adapters.push_back(std::make_unique<PacketCaptureAdapter>());
        adapters.push_back(std::make_unique<PresentationRenderAdapter>());
        for (auto& adapter : adapters) {
            const auto result = adapter->Install(modules);
            Diagnostics::LogLine(result.installed ? "info" : "warn",
                std::string(adapter->Name()) + "_install", NarrowAscii(result.detail));
        }
        // Last: only Watch needs it, so it never delays the scans the
        // overlay and the main-menu button wait on.
        std::wstring replayWorldDetail;
        const bool replayWorldInstalled = ReplayWorld::Install(replayWorldDetail);
        Diagnostics::LogLine(replayWorldInstalled ? "info" : "warn", "replay_world_install", NarrowAscii(replayWorldDetail));
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
    ReplayInjector::Shutdown();
    VideoRecorder::Shutdown();
    ReplayWorld::Remove();
    ScreenTelemetry::Remove();
    // Reverse installation order.
    for (auto adapter = adapters.rbegin(); adapter != adapters.rend(); ++adapter) (*adapter)->Remove();
    adapters.clear();
    PlayerSkinTelemetry::Remove();
    installed = false;
}

}
