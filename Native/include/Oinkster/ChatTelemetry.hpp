#pragma once

#include <string>

namespace Oinkster { class ModuleManager; }

namespace Oinkster::ChatTelemetry {

// Capability::ChatEvents. Hooks the client's chat-line sink - the function
// every TextPacket type handler (chat, whisper, announcement, system, ...)
// calls with the already formatted line - resolved by a unique .text
// signature, and publishes each line as a ChatEvent (message only; the sender
// is part of the formatted line). Read-only: the line still reaches the game's
// own chat unchanged. Pending until the first line.
[[nodiscard]] bool Install(ModuleManager& modules, std::wstring& detail) noexcept;
void Remove() noexcept;

}
