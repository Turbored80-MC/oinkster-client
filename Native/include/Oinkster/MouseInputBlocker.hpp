#pragma once

#include <string>

namespace Oinkster::MouseInputBlocker {

enum class InstallState { Initializing, Ready, Unavailable };

[[nodiscard]] bool Install(std::wstring& detail) noexcept;
[[nodiscard]] InstallState PollInstall(std::wstring& detail) noexcept;
[[nodiscard]] InstallState State() noexcept;
void SetActive(bool active) noexcept;
void Remove() noexcept;

}
