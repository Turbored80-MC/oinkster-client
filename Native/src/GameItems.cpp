#include "Oinkster/GameItems.hpp"
#include "Oinkster/SafeMemory.hpp"

// Derivations, all from Minecraft 1.26.5203's own code:
//  - Container::getItem for SimpleContainer (vtable 0xE6D69D0, slot 7 = RVA
//    0x19DF320) bounds the slot against the std::vector<ItemStack> at
//    this+0x1A0 and returns begin + slot * 0x98, else ItemStack::EMPTY_ITEM.
//  - The FillingContainer slot check at RVA 0x4AB4700 indexes the vector at
//    this+0x198 with the same 0x98 stride and compares the byte at +0x22
//    against EMPTY_ITEM's count.
//  - The Molang armor_damage_slot query (RVA 0x2213890) and
//    query.remaining_durability (RVA 0x2225980) read ItemStack+0x08 (the
//    Item WeakPtr, dereferenced once) and ItemStack+0x10 (the user-data
//    CompoundTag), then look up the "Damage" IntTag.
//  - CompoundTag lookup (RVA 0x2548B30) walks the MSVC std::map at tag+0x08:
//    root = head->parent, isnil at node+0x19, key std::string at node+0x20,
//    variant index at node+0x68 (3 = IntTag), tag at node+0x40.
//  - query.get_equipped_item_name (RVA 0x221B6F0) reads Item+0xD0, the
//    HashedString raw name id, whose std::string sits at +0xD8.
//  - Player::readAdditionalSaveData (RVA 0x1F1820) reaches PlayerInventory at
//    Player+0x5B8 and its Inventory at +0xB8; PlayerInventory::selectSlot
//    (RVA 0x1F4860) stores the hotbar slot (0-8) at +0x10.
//  - The ActorEquipmentComponent accessors (RVA 0x253BFF0 / 0x253C0D0) return
//    component+0 (hands, offhand at slot 1) and component+8 (armor, slots 0-3).
namespace Oinkster::GameItems {
namespace {
using namespace Layout;
constexpr unsigned MaxTreeDepth = 64;
}

bool ReadStackVector(const void* container, std::size_t itemsOffset, std::size_t maxSlots,
    std::uintptr_t& begin, std::size_t& count) noexcept {
    std::uintptr_t first{};
    std::uintptr_t last{};
    if (!container || !SafeMemory::ReadField(container, itemsOffset, first)
        || !SafeMemory::ReadField(container, itemsOffset + sizeof(void*), last)) {
        return false;
    }
    if (last < first || (last - first) % ItemStackSize != 0) return false;
    const std::size_t slots = (last - first) / ItemStackSize;
    if (slots > maxSlots || (slots != 0 && !first)) return false;
    begin = first;
    count = slots;
    return true;
}

bool ReadIntTag(const void* compoundTag, std::string_view key, int& value) {
    const void* head{};
    if (!compoundTag || !SafeMemory::ReadField(compoundTag, TagMapHead, head) || !head) return false;
    const void* node{};
    if (!SafeMemory::ReadField(head, MapNodeParent, node)) return false;
    for (unsigned depth = 0; depth < MaxTreeDepth && node; ++depth) {
        std::uint8_t isNil{};
        if (!SafeMemory::ReadField(node, MapNodeIsNil, isNil) || isNil != 0) return false;
        std::string nodeKey;
        if (!SafeMemory::ReadMsvcString(node, MapNodeKey, nodeKey, 128)) return false;
        const int order = std::string_view(nodeKey).compare(key);
        if (order == 0) {
            std::uint8_t index{};
            if (!SafeMemory::ReadField(node, MapNodeVariantIndex, index) || index != VariantIntTag) return false;
            return SafeMemory::ReadField(node, MapNodeValue + IntTagValue, value);
        }
        const void* next{};
        if (!SafeMemory::ReadField(node, order < 0 ? MapNodeRight : MapNodeLeft, next)) return false;
        node = next;
    }
    return false;
}

bool ReadItemRecord(const void* stack, Modules::ItemRecord& record) {
    record = {};
    const void* counter{};
    if (!stack || !SafeMemory::ReadField(stack, StackItem, counter)) return false;
    if (!counter) return true;  // no item: empty slot
    const void* item{};
    if (!SafeMemory::ReadField(counter, 0, item)) return false;
    if (!item) return true;
    std::uint8_t count{};
    if (!SafeMemory::ReadField(stack, StackCount, count)) return false;
    if (count == 0) return true;

    if (!SafeMemory::ReadMsvcString(item, ItemRawName, record.name, 96)) return false;
    std::string itemNamespace;
    if (SafeMemory::ReadMsvcString(item, ItemNamespace, itemNamespace, 64) && Modules::IsPlausibleIdentifier(itemNamespace)) {
        record.itemNamespace = std::move(itemNamespace);
    }
    record.count = count;

    const void* userData{};
    if (SafeMemory::ReadField(stack, StackUserData, userData) && userData) {
        int damage{};
        if (ReadIntTag(userData, "Damage", damage)) record.damage = damage;
    }
    return true;
}

}
