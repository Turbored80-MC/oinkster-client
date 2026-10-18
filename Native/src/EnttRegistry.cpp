#include "Oinkster/EnttRegistry.hpp"
#include "Oinkster/SafeMemory.hpp"

// Derived from Minecraft 1.26.5203's own inlined try_get (e.g. the function
// at RVA 0x20EFE0, which fetches StateVectorComponent, ActorRotationComponent
// and AABBShapeComponent for an Actor in `rcx`):
//
//   mov eax, [rcx+0x18]            ; EntityId
//   mov r9,  [rcx+0x10]            ; entt::basic_registry*
//   mov r8,  [r9+0x48] / [r9+0x50] ; pool buckets; count = (end-begin)/8
//   and edx, count-1 & <hash>      ; bucket
//   loop: idx = buckets[b] or node.next; -1 ends; node = [r9+0x68] + idx*0x20
//         until dword [node+8] == hash
//   storage = [node+0x10]
//   page = [storage+8][(entity & 0x3FFFF) >> 11]
//   stored = page[entity & 0x7FF]; ((entity & ~0x3FFFF) ^ stored) < 0x3FFFF
//   index = stored & 0x3FFFF
//   component = [storage+0x50][index >> 7] + (index & 0x7F) * sizeof(T)
namespace Oinkster::EnttRegistry {
namespace {
using namespace Layout;

// Bounds that no healthy registry comes near; anything past them is treated
// as a misread rather than walked.
constexpr std::size_t MaxPoolBuckets = 1U << 16;
constexpr std::size_t MaxPoolNodes = 1U << 16;
constexpr unsigned MaxChainSteps = 256;
constexpr std::size_t MaxSparsePages = (EntityMask + 1) / SparsePageEntries;

template <typename T>
bool Read(const void* base, std::size_t offset, T& value) noexcept { return SafeMemory::ReadField(base, offset, value); }

// begin/end pointer pair of a std::vector of `elementSize`-byte elements.
bool ReadVector(const void* owner, std::size_t beginOffset, std::size_t endOffset, std::size_t elementSize,
    std::uintptr_t& begin, std::size_t& count) noexcept {
    std::uintptr_t end{};
    if (!Read(owner, beginOffset, begin) || !Read(owner, endOffset, end)) return false;
    if (!begin || end < begin || (end - begin) % elementSize != 0) return false;
    count = (end - begin) / elementSize;
    return true;
}

const void* FindStorage(const void* registry, std::uint32_t typeHash) noexcept {
    std::uintptr_t buckets{};
    std::size_t bucketCount{};
    if (!ReadVector(registry, RegistryPoolBucketsBegin, RegistryPoolBucketsEnd, sizeof(std::size_t), buckets, bucketCount)) return nullptr;
    // entt sizes buckets to a power of two and masks with count - 1.
    if (bucketCount == 0 || bucketCount > MaxPoolBuckets || (bucketCount & (bucketCount - 1)) != 0) return nullptr;

    std::uintptr_t nodes{};
    std::size_t nodeCount{};
    if (!ReadVector(registry, RegistryPoolNodesBegin, RegistryPoolNodesEnd, PoolNodeSize, nodes, nodeCount)) return nullptr;
    if (nodeCount == 0 || nodeCount > MaxPoolNodes) return nullptr;

    std::size_t index{};
    const auto bucket = static_cast<std::size_t>(typeHash) & (bucketCount - 1);
    if (!Read(reinterpret_cast<const void*>(buckets), bucket * sizeof(std::size_t), index)) return nullptr;
    for (unsigned step = 0; step < MaxChainSteps; ++step) {
        if (index == static_cast<std::size_t>(-1) || index >= nodeCount) return nullptr;
        const auto* node = reinterpret_cast<const void*>(nodes + index * PoolNodeSize);
        std::uint32_t key{};
        if (!Read(node, PoolNodeKey, key)) return nullptr;
        if (key == typeHash) {
            const void* storage{};
            return Read(node, PoolNodeStorage, storage) ? storage : nullptr;
        }
        if (!Read(node, PoolNodeNext, index)) return nullptr;
    }
    return nullptr;
}
}

bool ReadActorEntity(const void* actor, EntityRef& entity) noexcept {
    EntityRef result{};
    if (!actor || !Read(actor, ActorRegistry, result.registry) || !result.registry) return false;
    if (!Read(actor, ActorEntity, result.entity)) return false;
    entity = result;
    return true;
}

bool ReadPoolEntities(const void* registry, std::uint32_t typeHash, std::size_t maxCount, std::vector<std::uint32_t>& entities) {
    entities.clear();
    if (!registry || maxCount == 0) return false;
    const void* storage = FindStorage(registry, typeHash);
    if (!storage) return false;
    std::uintptr_t packed{};
    std::size_t count{};
    if (!ReadVector(storage, StoragePackedBegin, StoragePackedEnd, sizeof(std::uint32_t), packed, count)) return false;
    if (count > EntityMask) return false;
    const std::size_t limit = count < maxCount ? count : maxCount;
    entities.reserve(limit);
    for (std::size_t index = 0; index < limit; ++index) {
        std::uint32_t entity{};
        if (!Read(reinterpret_cast<const void*>(packed), index * sizeof(entity), entity)) return false;
        // In-place-delete pools leave tombstones (entity bits all set) in place.
        if ((entity & EntityMask) == EntityMask) continue;
        entities.push_back(entity);
    }
    return true;
}

const void* FindComponent(const EntityRef& entity, std::uint32_t typeHash, std::size_t componentSize) noexcept {
    if (!entity.registry || componentSize == 0) return nullptr;
    const void* storage = FindStorage(entity.registry, typeHash);
    if (!storage) return nullptr;

    std::uintptr_t sparsePages{};
    std::size_t sparsePageCount{};
    if (!ReadVector(storage, StorageSparseBegin, StorageSparseEnd, sizeof(void*), sparsePages, sparsePageCount)) return nullptr;
    if (sparsePageCount > MaxSparsePages) return nullptr;
    const std::uint32_t entityIndex = entity.entity & EntityMask;
    const std::size_t pageIndex = entityIndex / SparsePageEntries;
    if (pageIndex >= sparsePageCount) return nullptr;
    const void* sparsePage{};
    if (!Read(reinterpret_cast<const void*>(sparsePages), pageIndex * sizeof(void*), sparsePage) || !sparsePage) return nullptr;
    std::uint32_t stored{};
    if (!Read(sparsePage, (entityIndex % SparsePageEntries) * sizeof(std::uint32_t), stored)) return nullptr;
    // Same membership test as entt::basic_sparse_set::contains: the stored
    // slot must carry this entity's version (tombstones and stale versions fail).
    if (((entity.entity & ~EntityMask) ^ stored) >= EntityMask) return nullptr;

    const std::uint32_t payloadIndex = stored & EntityMask;
    std::uintptr_t payloadPages{};
    std::size_t payloadPageCount{};
    if (!ReadVector(storage, StoragePayloadBegin, StoragePayloadEnd, sizeof(void*), payloadPages, payloadPageCount)) return nullptr;
    const std::size_t payloadPageIndex = payloadIndex / PayloadPageEntries;
    if (payloadPageIndex >= payloadPageCount) return nullptr;
    const void* payloadPage{};
    if (!Read(reinterpret_cast<const void*>(payloadPages), payloadPageIndex * sizeof(void*), payloadPage) || !payloadPage) return nullptr;
    const auto* component = static_cast<const std::uint8_t*>(payloadPage) + (payloadIndex % PayloadPageEntries) * componentSize;
    return SafeMemory::IsReadable(component, componentSize) ? component : nullptr;
}

}
