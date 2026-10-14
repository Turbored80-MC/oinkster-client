#pragma once

#include <string>
#include <cstdint>
#include <span>

namespace Oinkster { class ModuleManager; }

namespace Oinkster::Dx12Overlay {

[[nodiscard]] bool Install(ModuleManager& modules, std::wstring& error) noexcept;
[[nodiscard]] bool UpdatePlayerSkin(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgbaPixels, std::uint64_t revision) noexcept;
[[nodiscard]] bool ConsumeSettingsDirty() noexcept;
[[nodiscard]] bool ConsumeEjectRequested() noexcept;
void ConfigureControlKeys(std::uint32_t menuKey, std::uint32_t unloadKey) noexcept;
[[nodiscard]] std::uint32_t MenuKey() noexcept;
[[nodiscard]] std::uint32_t UnloadKey() noexcept;
// True while the ClickGUI owns input (clicks never reach gameplay).
[[nodiscard]] bool IsMenuVisible() noexcept;
void QueueMouseWheel(float delta) noexcept;
void Remove() noexcept;

}
