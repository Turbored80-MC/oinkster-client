#include "Oinkster/PlayerStateTelemetry.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/EnttRegistry.hpp"
#include "Oinkster/GameItems.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include "Oinkster/Modules/StatusLogic.hpp"
#include "Oinkster/Modules/TelemetryLogic.hpp"
#include <atomic>
#include <chrono>
#include <algorithm>
#include <format>
#include <optional>
#include <string>

namespace Oinkster::PlayerStateTelemetry {
namespace {

// Component layouts on 1.26.5203, each confirmed against the build's own code:
//  - ActorRotationComponent { Vec2 rotation; Vec2 previous; } (0x10): the
//    inlined try_get at RVA 0x20EFE0 indexes its pool with a 16-byte stride.
//    Bedrock's Vec2 rotation is (x = pitch, y = yaw).
//  - MobEffectsComponent { std::vector<MobEffectInstance>; } (0x18): RVA
//    0x205C63 reads begin/end out of a 24-byte element and divides by 0x90.
//  - MobEffectInstance (0x90): id at +0x0 (MobEffectInstance::getDisplayName,
//    RVA 0x2FE6CB0, bounds it against the 0x25-entry effect table), duration
//    in ticks at +0x4 (-1 is infinite; RVA 0x205C9D compares it against the
//    200-tick blink threshold), amplifier at +0x20 (getDisplayName appends
//    "potion.potency.<amplifier>" when 1 <= amplifier <= 5).
constexpr std::uint32_t RotationComponentHash = EnttRegistry::TypeHash("ActorRotationComponent");
constexpr std::uint32_t MobEffectsComponentHash = EnttRegistry::TypeHash("MobEffectsComponent");
constexpr std::size_t RotationComponentSize = 0x10;
constexpr std::size_t MobEffectsComponentSize = 0x18;
constexpr std::size_t MobEffectInstanceSize = 0x90;
constexpr std::size_t MobEffectId = 0x0;
constexpr std::size_t MobEffectDuration = 0x4;
constexpr std::size_t MobEffectAmplifier = 0x20;
// The vector is indexed by effect id, so its length tracks the registry size,
// not the number of active effects.
constexpr std::size_t MaxEffectSlots = 64;

constexpr std::uint32_t EquipmentComponentHash = EnttRegistry::TypeHash("ActorEquipmentComponent");
// Player inventory is 36 slots; armor containers hold 5 (head, chest, legs,
// feet, body); the hands container 2. Anything far past that is a misread.
constexpr std::size_t MaxContainerSlots = 64;
constexpr int HotbarSlots = 9;

constexpr auto EffectsInterval = std::chrono::milliseconds(200);
constexpr auto SampleLogInterval = std::chrono::seconds(5);

std::atomic<ModuleManager*> moduleManager{};
// Render-thread only.
bool rotationValidated{};
bool effectsValidated{};
bool equipmentValidated{};
bool inventoryValidated{};
std::string lastItemsSample;
SteadyTime lastEffectsPublished{};
SteadyTime lastSampleLogged{};
std::size_t lastEffectCount{static_cast<std::size_t>(-1)};

void Promote(ModuleManager& modules, Capability capability, bool& validated, std::string_view what) noexcept {
    if (validated) return;
    validated = true;
    try {
        modules.SetCapability(capability, {CapabilityState::Available, {}});
        Diagnostics::LogLine("info", "player_state_validated", std::string(what));
    } catch (...) {}
}

bool ReadRotation(const EnttRegistry::EntityRef& entity, float& pitch, float& yaw) noexcept {
    const void* component = EnttRegistry::FindComponent(entity, RotationComponentHash, RotationComponentSize);
    float rotation[2]{};
    if (!component || !SafeMemory::ReadField(component, 0, rotation)) return false;
    if (!Modules::IsPlausibleRotation(rotation[0], rotation[1])) return false;
    pitch = rotation[0];
    yaw = Modules::NormalizeYaw180(rotation[1]);
    return true;
}

bool ReadEffects(const EnttRegistry::EntityRef& entity, SteadyTime now, EffectsSnapshot& snapshot) {
    const void* component = EnttRegistry::FindComponent(entity, MobEffectsComponentHash, MobEffectsComponentSize);
    if (!component) return false;
    std::uintptr_t begin{};
    std::uintptr_t end{};
    if (!SafeMemory::ReadField(component, 0, begin) || !SafeMemory::ReadField(component, sizeof(void*), end)) return false;
    if (end < begin || (end - begin) % MobEffectInstanceSize != 0) return false;
    const std::size_t count = (end - begin) / MobEffectInstanceSize;
    if (count > MaxEffectSlots || (count != 0 && !begin)) return false;

    snapshot.capturedAt = now;
    snapshot.effects.clear();
    for (std::size_t index = 0; index < count; ++index) {
        const auto* instance = reinterpret_cast<const void*>(begin + index * MobEffectInstanceSize);
        Modules::MobEffectRecord record;
        if (!SafeMemory::ReadField(instance, MobEffectId, record.id)
            || !SafeMemory::ReadField(instance, MobEffectDuration, record.durationTicks)
            || !SafeMemory::ReadField(instance, MobEffectAmplifier, record.amplifier)) {
            return false;
        }
        if (auto effect = Modules::ToStatusEffect(record)) snapshot.effects.push_back(std::move(*effect));
    }
    return true;
}

// One stack out of a container's vector, or nullopt for an empty slot.
bool ReadSlot(std::uintptr_t begin, std::size_t count, std::size_t slot, std::optional<ItemStack>& stack) {
    stack.reset();
    if (slot >= count) return true;
    Modules::ItemRecord record;
    if (!GameItems::ReadItemRecord(reinterpret_cast<const void*>(begin + slot * GameItems::Layout::ItemStackSize), record)) return false;
    if (record.count != 0) stack = Modules::ToItemStack(record);
    return true;
}

bool ReadInventory(const void* player, SteadyTime now, InventorySnapshot& snapshot) {
    using namespace GameItems::Layout;
    const void* playerInventory{};
    if (!SafeMemory::ReadField(player, PlayerPlayerInventory, playerInventory) || !playerInventory) return false;
    int selected{};
    const void* inventory{};
    if (!SafeMemory::ReadField(playerInventory, PlayerInventorySelectedSlot, selected)
        || !SafeMemory::ReadField(playerInventory, PlayerInventoryInventory, inventory) || !inventory) {
        return false;
    }
    if (selected < 0 || selected >= HotbarSlots) return false;
    std::uintptr_t begin{};
    std::size_t count{};
    if (!GameItems::ReadStackVector(inventory, FillingContainerItems, MaxContainerSlots, begin, count)) return false;
    if (count < static_cast<std::size_t>(HotbarSlots)) return false;

    snapshot.capturedAt = now;
    snapshot.selectedHotbarSlot = selected;
    snapshot.slots.assign(std::min(count, Modules::MaxInventorySlots), std::nullopt);
    for (std::size_t slot = 0; slot < snapshot.slots.size(); ++slot) {
        if (!ReadSlot(begin, count, slot, snapshot.slots[slot])) return false;
    }
    return true;
}

bool ReadEquipment(const EnttRegistry::EntityRef& entity, SteadyTime now, EquipmentSnapshot& snapshot) {
    using namespace GameItems::Layout;
    const void* component = EnttRegistry::FindComponent(entity, EquipmentComponentHash, EquipmentComponentSize);
    if (!component) return false;
    const void* hands{};
    const void* armor{};
    if (!SafeMemory::ReadField(component, EquipmentHands, hands) || !SafeMemory::ReadField(component, EquipmentArmor, armor)
        || !hands || !armor) {
        return false;
    }
    std::uintptr_t armorBegin{};
    std::size_t armorCount{};
    std::uintptr_t handsBegin{};
    std::size_t handsCount{};
    if (!GameItems::ReadStackVector(armor, SimpleContainerItems, MaxContainerSlots, armorBegin, armorCount)
        || !GameItems::ReadStackVector(hands, SimpleContainerItems, MaxContainerSlots, handsBegin, handsCount)) {
        return false;
    }
    if (armorCount < ArmorSlotCount || handsCount <= static_cast<std::size_t>(OffhandSlot)) return false;

    snapshot.capturedAt = now;
    for (std::size_t slot = 0; slot < ArmorSlotCount; ++slot) {
        if (!ReadSlot(armorBegin, armorCount, slot, snapshot.armor[slot])) return false;
    }
    return ReadSlot(handsBegin, handsCount, static_cast<std::size_t>(OffhandSlot), snapshot.offHand);
}

std::string DescribeItems(const EquipmentSnapshot& equipment) {
    std::string text;
    auto add = [&text](std::string_view label, const std::optional<ItemStack>& stack) {
        text += std::format("{}{}=", text.empty() ? "" : " ", label);
        if (!stack) { text += "-"; return; }
        text += std::format("{}x{}", stack->itemId, stack->count);
        if (stack->durabilityRemaining) text += std::format("({}/{})", *stack->durabilityRemaining, *stack->durabilityMax);
    };
    add("head", equipment.armor[0]);
    add("chest", equipment.armor[1]);
    add("legs", equipment.armor[2]);
    add("feet", equipment.armor[3]);
    add("main", equipment.mainHand);
    add("off", equipment.offHand);
    return text;
}

}

void Attach(ModuleManager& modules) noexcept {
    rotationValidated = false;
    effectsValidated = false;
    equipmentValidated = false;
    inventoryValidated = false;
    lastItemsSample.clear();
    lastEffectsPublished = {};
    lastSampleLogged = {};
    lastEffectCount = static_cast<std::size_t>(-1);
    const std::wstring build(SupportedBuild::DisplayVersion);
    try {
        modules.SetCapability(Capability::LocalPlayerRotation, {CapabilityState::Pending,
            L"Waiting for the first validated ActorRotationComponent read in Minecraft " + build + L"."});
        modules.SetCapability(Capability::Effects, {CapabilityState::Pending,
            L"Waiting for the first validated MobEffectsComponent read in Minecraft " + build + L"."});
        modules.SetCapability(Capability::Equipment, {CapabilityState::Pending,
            L"Waiting for the first validated ActorEquipmentComponent read in Minecraft " + build + L"."});
        modules.SetCapability(Capability::Inventory, {CapabilityState::Pending,
            L"Waiting for the first validated player inventory read in Minecraft " + build + L"."});
    } catch (...) {}
    moduleManager.store(&modules, std::memory_order_release);
}

void Detach() noexcept {
    auto* modules = moduleManager.exchange(nullptr, std::memory_order_acq_rel);
    if (!modules) return;
    try {
        modules->SetCapability(Capability::LocalPlayerRotation, {CapabilityState::Unavailable, L"World telemetry was detached."});
        modules->SetCapability(Capability::Effects, {CapabilityState::Unavailable, L"World telemetry was detached."});
        modules->SetCapability(Capability::Equipment, {CapabilityState::Unavailable, L"World telemetry was detached."});
        modules->SetCapability(Capability::Inventory, {CapabilityState::Unavailable, L"World telemetry was detached."});
    } catch (...) {}
}

void Observe(const void* actor, PlayerTransform& transform, SteadyTime now) noexcept {
    auto* modules = moduleManager.load(std::memory_order_acquire);
    if (!modules || !actor) return;
    EnttRegistry::EntityRef entity;
    if (!EnttRegistry::ReadActorEntity(actor, entity)) return;

    float pitch{};
    float yaw{};
    const bool haveRotation = ReadRotation(entity, pitch, yaw);
    if (haveRotation) {
        transform.pitchDegrees = pitch;
        transform.yawDegrees = yaw;
        Promote(*modules, Capability::LocalPlayerRotation, rotationValidated, "ActorRotationComponent");
    }

    if (now - lastEffectsPublished >= EffectsInterval) {
        lastEffectsPublished = now;
        try {
            EffectsSnapshot snapshot;
            if (ReadEffects(entity, now, snapshot)) {
                Promote(*modules, Capability::Effects, effectsValidated, "MobEffectsComponent");
                if (snapshot.effects.size() != lastEffectCount) {
                    lastEffectCount = snapshot.effects.size();
                    std::string ids;
                    for (const auto& effect : snapshot.effects) ids += (ids.empty() ? "" : ",") + effect.effectId;
                    Diagnostics::LogLine("info", "player_state_effects", std::format("{} active [{}]", lastEffectCount, ids));
                }
                modules->Publish(snapshot);
            }

            // The hotbar selection doubles as the main hand, so equipment is
            // published after the inventory read it depends on.
            InventorySnapshot inventory;
            const bool haveInventory = ReadInventory(actor, now, inventory);
            if (haveInventory) {
                Promote(*modules, Capability::Inventory, inventoryValidated, "PlayerInventory");
                modules->Publish(inventory);
            }
            EquipmentSnapshot equipment;
            if (ReadEquipment(entity, now, equipment)) {
                if (haveInventory) equipment.mainHand = inventory.slots[static_cast<std::size_t>(inventory.selectedHotbarSlot)];
                Promote(*modules, Capability::Equipment, equipmentValidated, "ActorEquipmentComponent");
                modules->Publish(equipment);
                if (auto sample = DescribeItems(equipment); sample != lastItemsSample) {
                    Diagnostics::LogLine("info", "player_state_items", sample);
                    lastItemsSample = std::move(sample);
                }
            }
        } catch (...) {}
    }

    if (now - lastSampleLogged >= SampleLogInterval) {
        lastSampleLogged = now;
        try {
            Diagnostics::LogLine("info", "player_state_sample", haveRotation
                ? std::format("rotation pitch={:.1f} yaw={:.1f}", pitch, yaw)
                : std::string("rotation unreadable"));
        } catch (...) {}
    }
}

}
