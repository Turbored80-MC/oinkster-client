#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Oinkster { class ModuleManager; }

// Capability::PacketSend: hands client->server packets to the game's own
// network peer, the way the game does after serializing one.
//
// BatchedNetworkPeer::sendPacket on 1.26.5203 (RVA 0x46EB60, vtable slot 1
// of the peer at NetworkConnection+0x118, unique .text signature):
//   void (NetworkPeer* this, const std::string& packet, Reliability, Compressibility)
// It appends the packet (header varint + body, the same bytes the receive
// side decodes) to the outgoing batch; reliability is ignored and
// compressibility 0 means "compress with the batch". It only reads the string.
//
// Layouts the game serializes through cereal reflection (CommandRequest:
// Command, Origin, IsInternal, Version; ModalFormResponse: id, response) are
// never guessed: the hook watches the player's own packets and keeps the
// bytes around the command / response as a template (persisted per build in
// %LOCALAPPDATA%\Oinkster\Client\packet-templates.txt). Until the player has
// sent one command (and answered one form), the matching Queue call refuses.
//
// Packets are only written on the network thread, from inside the receive
// hook (Poll), and only through a peer whose vtable slot 1 is the validated
// sendPacket - so never concurrently with the game's own sends.
namespace Oinkster::PacketSender {

[[nodiscard]] bool Install(ModuleManager& modules, std::wstring& detail) noexcept;
void Remove() noexcept;

// ReplayPacketBridge calls this from NetworkConnection::receivePacket for
// the connection carrying server packets; queued packets are sent here.
void Poll(void* serverConnection) noexcept;

[[nodiscard]] bool CanSendCommands() noexcept;
[[nodiscard]] bool CanSendFormResponses() noexcept;
// "/q sky" etc. False (nothing queued) without a learned template.
bool QueueCommand(std::string_view command) noexcept;
// Answers form `formId` with `response` (a button index "0", or JSON).
bool QueueFormResponse(std::uint32_t formId, std::string_view response) noexcept;
// A packet whose layout the caller built from the server's own bytes (e.g.
// PlayerSkin from a PlayerList entry). False until the peer validated.
bool QueuePacket(std::vector<std::uint8_t> packet) noexcept;

// Pure template logic, unit-tested.
struct CommandTemplate {
    std::vector<std::uint8_t> header;  // packet header varint
    std::vector<std::uint8_t> suffix;  // everything after the command string
};
struct FormTemplate {
    std::vector<std::uint8_t> header;
    std::vector<std::uint8_t> beforeResponse;  // after the form id, before the response string
    std::vector<std::uint8_t> suffix;          // after the response string
};
inline constexpr std::uint32_t CommandRequestId = 77;
inline constexpr std::uint32_t ModalFormResponseId = 101;
[[nodiscard]] std::optional<CommandTemplate> LearnCommandTemplate(std::span<const std::uint8_t> packet);
[[nodiscard]] std::vector<std::uint8_t> BuildCommand(const CommandTemplate& learned, std::string_view command);
[[nodiscard]] std::optional<FormTemplate> LearnFormTemplate(std::span<const std::uint8_t> packet);
[[nodiscard]] std::vector<std::uint8_t> BuildFormResponse(const FormTemplate& learned, std::uint32_t formId, std::string_view response);
[[nodiscard]] std::string EncodeTemplates(const std::optional<CommandTemplate>& command, const std::optional<FormTemplate>& form,
    std::string_view build);
void DecodeTemplates(std::string_view text, std::string_view build, std::optional<CommandTemplate>& command,
    std::optional<FormTemplate>& form);

}
