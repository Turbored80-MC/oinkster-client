#include "Oinkster/Capabilities.hpp"
#include "Oinkster/SupportedBuild.hpp"

namespace Oinkster {
namespace {
struct CapabilityInfo {
    std::string_view id;
    std::wstring_view name;
    bool statically;
};

constexpr std::array<CapabilityInfo, CapabilityCount> Infos{{
    {"render-metrics", L"render metrics", true},
    {"frame-composition", L"frame composition", true},
    {"local-input", L"local input", true},
    {"input-synthesis", L"local input synthesis", true},
    {"input-filtering", L"reviewed mouse input filtering", true},
    {"dc-prevent-policy", L"DC Prevent server policy", true},
    {"cps-cap-policy", L"CPS Cap server policy", true},
    // Reviewed WorldTelemetry implementation for 1.26.5203. It remains
    // Pending until its unique signature validates in the live process.
    {"local-player-position", L"local player position", true},
    // PlayerStateTelemetry reads these from the same validated hook's Actor
    // through the entt registry; Pending until the first plausible read.
    {"local-player-rotation", L"local player rotation", true},
    {"inventory", L"inventory snapshot", true},
    {"equipment", L"equipment snapshot", true},
    {"effects", L"status effects", true},
    // SessionTelemetry: RakPeer ping ring and LevelData::mLevelName, each
    // Pending until its first plausible read.
    {"network-metrics", L"network metrics", true},
    {"server-state", L"server state", true},
    // ResourcePackTelemetry: the ILevel ResourcePackManager stacks.
    {"resource-packs", L"resource packs", true},
    // CombatTelemetry: MobHurtTimeComponent of the _renderLevelPrep Actor
    // and the entity a click was aimed at; Pending until the first read.
    {"combat-events", L"combat events", true},
    // PresentationAdapter: camera FOV / view bobbing, gamma and time of day
    // are real hooks; the others are unlocked for testing without one.
    {"selected-block-render", L"selected-block render data", true},
    {"damage-render", L"damage render data", true},
    // SoundTelemetry: the LevelRenderer world-sound helper.
    {"sound-events", L"sound events", true},
    // ChatTelemetry: the client chat-line sink.
    {"chat-events", L"chat events", true},
    {"camera-presentation", L"camera presentation", true},
    {"camera-rotation", L"camera rotation presentation", true},
    {"camera-motion", L"camera motion presentation", true},
    {"third-person-presentation", L"third-person camera presentation", true},
    {"lighting-presentation", L"local lighting presentation", true},
    {"time-presentation", L"local time presentation", true},
    {"weather-presentation", L"local weather presentation", true},
    {"screen-effects-presentation", L"screen-effect presentation", true},
    {"hand-presentation", L"hand presentation", true},
    // ReplayPacketBridge: decoded server->client packets for Replay Mod.
    {"packet-capture", L"incoming packet capture", true},
    {"perspective-presentation", L"camera perspective presentation", true},
    {"packet-send", L"client packet sending", true},
}};

const CapabilityInfo& Info(Capability capability) noexcept {
    const auto index = static_cast<std::size_t>(capability);
    return Infos[index < Infos.size() ? index : 0];
}

std::wstring Build() { return std::wstring(SupportedBuild::DisplayVersion); }
}

std::string_view CapabilityId(Capability capability) noexcept { return Info(capability).id; }
std::wstring_view CapabilityName(Capability capability) noexcept { return Info(capability).name; }
bool IsCapabilitySupported(Capability capability) noexcept { return Info(capability).statically; }

std::optional<Capability> ParseCapabilityId(std::string_view id) noexcept {
    for (std::size_t index = 0; index < Infos.size(); ++index) {
        if (Infos[index].id == id) return static_cast<Capability>(index);
    }
    return std::nullopt;
}

std::string_view CapabilityStateId(CapabilityState state) noexcept {
    switch (state) {
    case CapabilityState::Pending: return "pending";
    case CapabilityState::Available: return "available";
    case CapabilityState::Unavailable: return "unavailable";
    }
    return "unavailable";
}

CapabilityStatus DefaultCapabilityStatus(Capability capability) {
    switch (capability) {
    case Capability::RenderMetrics:
    case Capability::FrameComposition:
        return {CapabilityState::Available, {}};
    case Capability::LocalInput:
    case Capability::InputSynthesis:
        return {CapabilityState::Available, {}};
    case Capability::InputFiltering:
        return {CapabilityState::Pending, L"Waiting for the reviewed Minecraft mouse queue hook to validate."};
    case Capability::DcPreventPolicy:
        return {CapabilityState::Pending, L"Waiting for the current server's DC Prevent policy."};
    case Capability::CpsCapPolicy:
        return {CapabilityState::Pending, L"Waiting for the current server's CPS Cap policy."};
    case Capability::LocalPlayerPosition:
        return {CapabilityState::Pending,
            L"Waiting for the world-telemetry signature to validate against Minecraft " + Build() + L"."};
    case Capability::LocalPlayerRotation:
        return {CapabilityState::Pending,
            L"Waiting for the first validated ActorRotationComponent read in Minecraft " + Build() + L"."};
    case Capability::Inventory:
        return {CapabilityState::Pending,
            L"Waiting for the first validated player inventory read in Minecraft " + Build() + L"."};
    case Capability::Equipment:
        return {CapabilityState::Pending,
            L"Waiting for the first validated ActorEquipmentComponent read in Minecraft " + Build() + L"."};
    case Capability::Effects:
        return {CapabilityState::Pending,
            L"Waiting for the first validated MobEffectsComponent read in Minecraft " + Build() + L"."};
    case Capability::NetworkMetrics:
        return {CapabilityState::Pending, L"Waiting for a connected RakNet peer in Minecraft " + Build() + L"."};
    case Capability::ServerState:
        return {CapabilityState::Pending, L"Waiting for the first validated LevelData read in Minecraft " + Build() + L"."};
    case Capability::ResourcePacks:
        return {CapabilityState::Pending, L"Waiting for the first validated resource-pack stack read in Minecraft " + Build() + L"."};
    case Capability::CombatEvents:
        return {CapabilityState::Pending,
            L"Waiting for the first validated MobHurtTimeComponent read in Minecraft " + Build() + L"."};
    case Capability::SelectedBlockRender:
        return {CapabilityState::Pending, L"Waiting for the block hit result to validate in Minecraft " + Build() + L"."};
    case Capability::DamageRender:
        return {CapabilityState::Pending, L"Waiting for the actor hurt color hook to validate in Minecraft " + Build() + L"."};
    case Capability::SoundEvents:
        return {CapabilityState::Pending, L"Waiting for the first world sound in Minecraft " + Build() + L"."};
    case Capability::ChatEvents:
        return {CapabilityState::Pending, L"Waiting for the first chat line in Minecraft " + Build() + L"."};
    case Capability::CameraPresentation:
        return {CapabilityState::Pending, L"Waiting for the camera FOV hook to validate in Minecraft " + Build() + L"."};
    case Capability::CameraRotation:
        return {CapabilityState::Pending, L"Waiting for the camera rotation hook to validate in Minecraft " + Build() + L"."};
    case Capability::CameraMotion:
        return {CapabilityState::Pending, L"Waiting for the camera rotation hook to validate in Minecraft " + Build() + L"."};
    case Capability::ThirdPersonPresentation:
        return {CapabilityState::Pending, L"Waiting for the camera position hook to validate in Minecraft " + Build() + L"."};
    case Capability::LightingPresentation:
        return {CapabilityState::Pending, L"Waiting for the gamma hook to validate in Minecraft " + Build() + L"."};
    case Capability::TimePresentation:
        return {CapabilityState::Pending, L"Waiting for the time-of-day hook to validate in Minecraft " + Build() + L"."};
    case Capability::WeatherPresentation:
        return {CapabilityState::Pending, L"Waiting for the weather hook to validate in Minecraft " + Build() + L"."};
    case Capability::ScreenEffectsPresentation:
        return {CapabilityState::Pending, L"Waiting for the vignette hook to validate in Minecraft " + Build() + L"."};
    case Capability::HandPresentation:
        return {CapabilityState::Pending, L"Waiting for the item-in-hand hook to validate in Minecraft " + Build() + L"."};
    case Capability::PacketCapture:
        return {CapabilityState::Pending, L"Waiting for the first incoming game packet in Minecraft " + Build() + L"."};
    case Capability::PerspectivePresentation:
        return {CapabilityState::Pending, L"Waiting for the camera perspective hook to validate in Minecraft " + Build() + L"."};
    case Capability::PacketSend:
        return {CapabilityState::Pending, L"Waiting for a server connection in Minecraft " + Build() + L"."};
    case Capability::Count:
        break;
    }
    return {CapabilityState::Unavailable, L"Unknown capability."};
}

}
