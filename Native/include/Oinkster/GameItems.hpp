#pragma once

#include "Oinkster/Modules/StatusLogic.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// Guarded, read-only access to Bedrock ItemStacks and containers on
// 1.26.5203. Pure memory reads through SafeMemory: no Item/Container virtual
// is ever called. Layouts and their derivations are documented in
// GameItems.cpp; Layout is exposed for the unit test that builds fake stacks.
// ---------------------------------------------------------------------------
namespace Oinkster::GameItems {

namespace Layout {
// ItemStack / ItemStackBase (0x98 bytes).
inline constexpr std::size_t ItemStackSize = 0x98;
inline constexpr std::size_t StackItem = 0x08;      // WeakPtr<Item>: SharedCounter* whose +0 is Item*
inline constexpr std::size_t StackUserData = 0x10;  // CompoundTag*
inline constexpr std::size_t StackCount = 0x22;     // uint8
// Item.
inline constexpr std::size_t ItemRawName = 0xD8;    // std::string inside HashedString mRawNameId (+0xD0)
inline constexpr std::size_t ItemNamespace = 0x100; // std::string mNamespace
// CompoundTag: std::map<std::string, CompoundTagVariant> at +0x08.
inline constexpr std::size_t TagMapHead = 0x08;
inline constexpr std::size_t MapNodeLeft = 0x00;
inline constexpr std::size_t MapNodeParent = 0x08;
inline constexpr std::size_t MapNodeRight = 0x10;
inline constexpr std::size_t MapNodeIsNil = 0x19;
inline constexpr std::size_t MapNodeKey = 0x20;      // std::string
inline constexpr std::size_t MapNodeValue = 0x40;    // the Tag inside CompoundTagVariant
inline constexpr std::size_t MapNodeVariantIndex = 0x68;
inline constexpr std::uint8_t VariantIntTag = 3;
inline constexpr std::size_t IntTagValue = 0x08;
// Containers: std::vector<ItemStack> members.
inline constexpr std::size_t SimpleContainerItems = 0x1A0;
inline constexpr std::size_t FillingContainerItems = 0x198;
// Player -> PlayerInventory -> Inventory.
inline constexpr std::size_t PlayerPlayerInventory = 0x5B8;
inline constexpr std::size_t PlayerInventorySelectedSlot = 0x10;
inline constexpr std::size_t PlayerInventoryInventory = 0xB8;
// ActorEquipmentComponent { SimpleContainer* hands; SimpleContainer* armor; }.
inline constexpr std::size_t EquipmentHands = 0x00;
inline constexpr std::size_t EquipmentArmor = 0x08;
inline constexpr std::size_t EquipmentComponentSize = 0x10;
inline constexpr int OffhandSlot = 1;
}

// Reads the stack at `stack`. Returns false only when the read itself fails
// (unreadable memory, implausible header); an empty slot returns true with
// `record.count == 0`.
[[nodiscard]] bool ReadItemRecord(const void* stack, Modules::ItemRecord& record);

// begin/count of the std::vector<ItemStack> at container+itemsOffset, bounded
// by maxSlots.
[[nodiscard]] bool ReadStackVector(const void* container, std::size_t itemsOffset, std::size_t maxSlots,
    std::uintptr_t& begin, std::size_t& count) noexcept;

// The value of an IntTag child of `compoundTag` named `key`.
[[nodiscard]] bool ReadIntTag(const void* compoundTag, std::string_view key, int& value);

}
