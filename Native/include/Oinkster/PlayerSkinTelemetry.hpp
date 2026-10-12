#pragma once

#include <string>

namespace Oinkster::PlayerSkinTelemetry {

// Installs the bundled Oinkster atlas immediately, then enables the reviewed
// classic-skin reader for the supported Bedrock build. Failure is non-fatal:
// the embedded atlas remains active.
[[nodiscard]] bool Install(std::wstring& detail) noexcept;
void ObserveClientInstance(void* clientInstance) noexcept;
// Receives the Actor supplied to the already-validated
// InGamePlayScreen::_renderLevelPrep hook. The implementation verifies RTTI
// before treating it as the local player, then inspects only that object's
// pointer members for a structurally valid equipped skin.
void ObserveLocalPlayer(void* localPlayer) noexcept;
// Called on the menu-open edge. Live skin reads are gated on this: neither
// ObserveLocalPlayer nor the background worker touches game memory until the
// menu has been opened, because those reads caused gameplay frame spikes.
void RequestRefresh() noexcept;
// Called on the menu-close edge. Stops any in-progress background pass so
// nothing keeps scanning game memory during gameplay.
void NotifyMenuClosed() noexcept;
void Remove() noexcept;

}
