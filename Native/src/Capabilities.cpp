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
    {"resource-packs", L"resource packs", false},
    {"combat-events", L"combat events", false},
    {"selected-block-render", L"selected-block render data", false},
    {"damage-render", L"damage render data", false},
    {"sound-events", L"sound events", false},
    {"chat-events", L"chat events", false},
    {"camera-presentation", L"camera presentation", false},
    {"hand-presentation", L"hand presentation", false},
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
        return {CapabilityState::Unavailable, L"No validated resource-pack stack reader exists for Minecraft " + Build() + L"."};
    case Capability::CombatEvents:
        return {CapabilityState::Unavailable,
            L"No validated authoritative hit/damage event source exists for Minecraft " + Build()
            + L"; local clicks are never treated as confirmed hits."};
    case Capability::SelectedBlockRender:
        return {CapabilityState::Unavailable, L"No validated selected-block outline render hook exists for Minecraft " + Build() + L"."};
    case Capability::DamageRender:
        return {CapabilityState::Unavailable, L"No validated damage-tint render hook exists for Minecraft " + Build() + L"."};
    case Capability::SoundEvents:
        return {CapabilityState::Unavailable, L"No validated sound-event source exists for Minecraft " + Build() + L"."};
    case Capability::ChatEvents:
        return {CapabilityState::Unavailable, L"No validated chat-message source exists for Minecraft " + Build() + L"."};
    case Capability::CameraPresentation:
        return {CapabilityState::Unavailable, L"No validated camera/FOV render hook exists for Minecraft " + Build() + L"."};
    case Capability::HandPresentation:
        return {CapabilityState::Unavailable, L"No validated first-person hand render hook exists for Minecraft " + Build() + L"."};
    case Capability::Count:
        break;
    }
    return {CapabilityState::Unavailable, L"Unknown capability."};
}

}
