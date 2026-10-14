#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Guarded, read-only lookups in Bedrock's entt component registry.
//
// Since 1.20.50 most per-actor state (rotation, mob effects, equipment, ...)
// lives in entt component pools instead of Actor fields. Every lookup here
// walks the registry exactly the way the game's own inlined
// entt::basic_registry::try_get does on 1.26.5203 (see EnttRegistry.cpp for
// the derivation), but through SafeMemory, with every index bounded, so a
// changed layout yields "not found" instead of a fault.
//
// A pool is keyed by entt::type_hash<T>, which is FNV-1a over the type's
// stripped name. 1.26.5203 is clang-compiled, so the name is the bare type
// name ("ActorRotationComponent"), not MSVC's "struct ActorRotationComponent"
// that third-party tables use; the bare-name hashes are the ones that occur
// as immediates throughout the build's .text.
// ---------------------------------------------------------------------------
namespace Oinkster::EnttRegistry {

[[nodiscard]] constexpr std::uint32_t TypeHash(std::string_view typeName) noexcept {
    std::uint32_t hash = 2166136261U;
    for (const char character : typeName) {
        hash = (hash ^ static_cast<std::uint8_t>(character)) * 16777619U;
    }
    return hash;
}

// The registry and entity id an Actor's EntityContext (Actor+0x8) refers to.
struct EntityRef {
    const void* registry{};
    std::uint32_t entity{};
};

[[nodiscard]] bool ReadActorEntity(const void* actor, EntityRef& entity) noexcept;

// Address of `entity`'s component in the pool keyed by `typeHash`, whose
// elements are `componentSize` bytes, or nullptr when the pool, the entity or
// any pointer along the way is missing or implausible. The returned address
// is game memory: read it only through SafeMemory.
[[nodiscard]] const void* FindComponent(const EntityRef& entity, std::uint32_t typeHash, std::size_t componentSize) noexcept;

// Up to `maxCount` live entities that own a component in the pool keyed by
// `typeHash`, in the pool's packed order. Tombstones are skipped; false when
// the pool or its packed vector is missing or implausible.
[[nodiscard]] bool ReadPoolEntities(const void* registry, std::uint32_t typeHash, std::size_t maxCount,
    std::vector<std::uint32_t>& entities);

// Layout constants, exposed for the unit test that builds a fake registry.
namespace Layout {
// entt::basic_registry: pools (dense_map<id_type, shared_ptr<sparse_set>>).
inline constexpr std::size_t RegistryPoolBucketsBegin = 0x48;  // std::vector<size_t> sparse
inline constexpr std::size_t RegistryPoolBucketsEnd = 0x50;
inline constexpr std::size_t RegistryPoolNodesBegin = 0x68;    // std::vector<dense_map_node> packed
inline constexpr std::size_t RegistryPoolNodesEnd = 0x70;
// internal::dense_map_node<uint32_t, shared_ptr<...>>: { size_t next; pair<key, value> }
inline constexpr std::size_t PoolNodeSize = 0x20;
inline constexpr std::size_t PoolNodeNext = 0x00;
inline constexpr std::size_t PoolNodeKey = 0x08;
inline constexpr std::size_t PoolNodeStorage = 0x10;           // shared_ptr::get()
// basic_sparse_set: { vptr; vector<entity*> sparse pages; ... }; basic_storage
// adds its own vector<T*> payload pages at +0x50.
inline constexpr std::size_t StorageSparseBegin = 0x08;
inline constexpr std::size_t StorageSparseEnd = 0x10;
// vector<EntityId> packed (basic_sparse_set::push_back at RVA 0x21FF90 indexes
// it at +0x20/+0x28 with a 4-byte stride).
inline constexpr std::size_t StoragePackedBegin = 0x20;
inline constexpr std::size_t StoragePackedEnd = 0x28;
inline constexpr std::size_t StoragePayloadBegin = 0x50;
inline constexpr std::size_t StoragePayloadEnd = 0x58;
// EntityId traits: 18-bit entity, 14-bit version, 2048-entry sparse pages.
inline constexpr std::uint32_t EntityMask = 0x3FFFF;
inline constexpr std::uint32_t SparsePageEntries = 2048;
// entt::component_traits<T>::page_size for Bedrock's in-place-delete components.
inline constexpr std::uint32_t PayloadPageEntries = 128;
// Actor: EntityContext { EntityRegistry&; basic_registry&; EntityId }.
inline constexpr std::size_t ActorRegistry = 0x10;
inline constexpr std::size_t ActorEntity = 0x18;
}

}
