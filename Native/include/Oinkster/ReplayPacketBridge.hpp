#pragma once

#include <string>

namespace Oinkster { class ModuleManager; }

namespace Oinkster::ReplayPacketBridge {

// Capability::PacketCapture. Hooks NetworkConnection::receivePacket - the
// call the game's decode loop makes to pull the next already decrypted and
// decompressed packet (header varint + body) off a connection - resolved by a
// unique .text signature. Each connection's direction is decided from the
// packet ids it carries; only server->client packets are ever copied and
// published (as ReplayPacketEvent), so nothing the client sends - its login
// identity included - is captured. Read-only: every packet still reaches the
// game unchanged. Pending until the first server packet.
[[nodiscard]] bool Install(ModuleManager& modules, std::wstring& detail) noexcept;
void Remove() noexcept;

}
