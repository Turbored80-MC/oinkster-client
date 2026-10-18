#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Layout-independent halves of the live equipped-skin reader.
//
// Everything here is pure arithmetic over values that have *already* been read
// out of the Minecraft process, so it is unit-testable without a game attached
// (see Native/tests/NativeTests.cpp). The reader itself lives in
// PlayerSkinTelemetry.cpp and is the only thing that touches game memory.
//
// Provenance of every constant below is one of:
//   [flarial] Native/out/reference/flarial-dll-oss/src/SDK/Client/Util/mce.hpp
//             and .../src/SDK/Client/Level/Level.hpp - Mojang's own field order
//             as reverse-engineered and maintained upstream.
//   [probe]   Measured from MSVC 14.44 with the *release* STL configuration
//             (/MD /O2, _ITERATOR_DEBUG_LEVEL=0), i.e. the configuration
//             Minecraft.Windows is built with. This DLL is built Debug, so its
//             own std:: types have different layouts and must never be used to
//             read the game's containers.
namespace Oinkster::PlayerSkinLayout {

// ---------------------------------------------------------------------------
// MSVC release std::string  [probe: sizeof == 32]
// ---------------------------------------------------------------------------
constexpr std::size_t StringBufferOffset = 0x00;
constexpr std::size_t StringSizeOffset = 0x10;
constexpr std::size_t StringCapacityOffset = 0x18;
constexpr std::size_t StringSize = 0x20;
// Short-string optimisation keeps up to 15 chars plus a NUL in the 16-byte
// buffer, so capacity is exactly 15 until the string goes to the heap.
constexpr std::uint64_t StringSsoCapacity = 15;

// ---------------------------------------------------------------------------
// MSVC release std::unordered_map  [probe: sizeof == 64]
//
//   +0x00  float  _Max_bucket_size (+ padding)
//   +0x08  void*  list sentinel node   <- NOT at +0x00
//   +0x10  size_t element count
//   +0x18  bucket vector { _Myfirst, _Mylast, _Myend }
//   +0x30  size_t _Mask
//
// The sentinel being at +0x08 rather than +0x00 is the whole reason the
// previous reader never got past the player map: reading { sentinel, size }
// from the map base yielded { 0x3f800000, <heap pointer> }, so the size looked
// like a ~47-bit integer and every poll bailed out as "map unreadable".
// ---------------------------------------------------------------------------
constexpr std::size_t MapListHeadOffset = 0x08;
constexpr std::size_t MapListSizeOffset = 0x10;
constexpr std::size_t MapBucketFirstOffset = 0x18;
constexpr std::size_t MapBucketLastOffset = 0x20;
constexpr std::size_t MapBucketEndOffset = 0x28;
constexpr std::size_t MapMaskOffset = 0x30;
constexpr std::size_t MapSize = 0x40;

// MSVC release std::list node  [probe: value at node + 0x10]
constexpr std::size_t NodeNextOffset = 0x00;
constexpr std::size_t NodePrevOffset = 0x08;
constexpr std::size_t NodeValueOffset = 0x10;

// The map's value_type is pair<const mcUUID, PlayerListEntry_1_21_90>; mcUUID
// is two uint64s, so the entry starts one 16-byte key after the node value.
constexpr std::size_t UuidSize = 0x10;
constexpr std::size_t NodeEntryOffset = NodeValueOffset + UuidSize;

// A Bedrock player list is bounded by the server's player cap. Anything past
// this is a misread offset, not a very busy Realm.
constexpr std::uint64_t MaxPlayerMapEntries = 1024;

// ---------------------------------------------------------------------------
// Blob  [flarial mce.hpp; probe for the unique_ptr interior]
//
// Blob::mBlob is unique_ptr<unsigned char[], Blob::Deleter>, and Deleter holds
// a function pointer - so it is *not* empty and MSVC's _Compressed_pair cannot
// elide it. The deleter therefore occupies slot 0 and the owned pointer slot 1.
// That is what puts the pixels at Image+0x20 rather than Image+0x18.
// ---------------------------------------------------------------------------
constexpr std::size_t BlobDeleterOffset = 0x00;
constexpr std::size_t BlobPointerOffset = 0x08;
constexpr std::size_t BlobSizeOffset = 0x10;
constexpr std::size_t BlobSize = 0x18;

// ---------------------------------------------------------------------------
// mce::Image  [flarial mce.hpp; probe: sizeof == 48, Blob at +0x18]
// ---------------------------------------------------------------------------
constexpr std::size_t ImageFormatOffset = 0x00;
constexpr std::size_t ImageWidthOffset = 0x04;
constexpr std::size_t ImageHeightOffset = 0x08;
constexpr std::size_t ImageDepthOffset = 0x0C;
constexpr std::size_t ImageUsageOffset = 0x10;
constexpr std::size_t ImageBlobOffset = 0x18;
constexpr std::size_t ImageSize = 0x30;

constexpr std::size_t ImagePixelsOffset = ImageBlobOffset + BlobPointerOffset;  // 0x20
constexpr std::size_t ImageByteCountOffset = ImageBlobOffset + BlobSizeOffset;  // 0x28

// Bedrock has used both values for four-channel, 8-bit skin images across
// builds. The SDK enum still calls 3 RGBA8Unorm, while the current player-head
// path in the maintained reference observes 4 for the same byte layout.
constexpr std::int32_t RgbaUnormFormat = 3;
constexpr std::int32_t RgbaUnormSrgbFormat = 4;

// ---------------------------------------------------------------------------
// PlayerSkin  [flarial mce.hpp - annotated inline by upstream]
//
// Corroboration that the chain is intact: mPieceTintColors is an
// unordered_map at +0x1B8 and mSkinColor an MCCColor (four floats) at +0x1F8.
// 0x1B8 + 0x40 == 0x1F8 exactly matches the probed sizeof(unordered_map), and
// 0x1F8 + 0x10 == 0x208 lands on the flag block. Both independent checks agree.
// ---------------------------------------------------------------------------
constexpr std::size_t SkinIdOffset = 0x00;
constexpr std::size_t SkinPlayFabIdOffset = 0x20;
constexpr std::size_t SkinFullIdOffset = 0x40;
constexpr std::size_t SkinResourcePatchOffset = 0x60;
constexpr std::size_t SkinDefaultGeometryNameOffset = 0x80;
constexpr std::size_t SkinImageOffset = 0xA0;
constexpr std::size_t CapeImageOffset = 0xD0;
constexpr std::size_t SkinArmSizeOffset = 0x1B0;
constexpr std::size_t SkinPieceTintColorsOffset = 0x1B8;
constexpr std::size_t SkinColorOffset = 0x1F8;
constexpr std::size_t SkinTrustedFlagOffset = 0x208;
constexpr std::size_t SkinPremiumOffset = 0x209;
constexpr std::size_t SkinPersonaOffset = 0x20A;
constexpr std::size_t SkinPersonaCapeOnClassicOffset = 0x20B;
constexpr std::size_t SkinPrimaryUserOffset = 0x20C;
constexpr std::size_t SkinOverridesAppearanceOffset = 0x20D;
constexpr std::size_t PlayerSkinSize = 0x210;

static_assert(SkinPieceTintColorsOffset + MapSize == SkinColorOffset,
    "PlayerSkin::mPieceTintColors is an unordered_map immediately before mSkinColor");
static_assert(SkinImageOffset + ImageSize == CapeImageOffset,
    "PlayerSkin::mSkinImage is an mce::Image immediately before mCapeImage");
static_assert(SkinDefaultGeometryNameOffset + StringSize == SkinImageOffset,
    "PlayerSkin's five leading std::strings must end exactly at mSkinImage");

// ---------------------------------------------------------------------------
// PlayerListEntry_1_21_90  [flarial Level.hpp]
//
// Modern Bedrock does not embed PlayerSkin in the list entry any more; it
// holds shared_ptr<Bedrock::Application::ThreadOwner<PlayerSkin>>. The
// shared_ptr's first slot is the object pointer [probe], and ThreadOwner
// declares mObject first, so one dereference lands on the PlayerSkin.
// ---------------------------------------------------------------------------
constexpr std::size_t EntryIdOffset = 0x00;
constexpr std::size_t EntryUuidOffset = 0x08;
constexpr std::size_t EntryNameOffset = 0x18;
constexpr std::size_t EntryXuidOffset = 0x38;
constexpr std::size_t EntryPlatformOnlineIdOffset = 0x58;
constexpr std::size_t EntryBuildPlatformOffset = 0x78;
constexpr std::size_t EntrySkinHandleOffset = 0x80;

constexpr std::size_t SharedPtrObjectOffset = 0x00;
constexpr std::size_t ThreadOwnerObjectOffset = 0x00;

[[nodiscard]] constexpr std::uint64_t ThreadOwnedSkinAddress(std::uint64_t ownerAddress) noexcept {
    return ownerAddress == 0 ? 0 : ownerAddress + ThreadOwnerObjectOffset;
}

static_assert(EntryNameOffset + 3 * StringSize == EntryBuildPlatformOffset,
    "PlayerListEntry's name/XUID/platformOnlineId strings must end at buildPlatform");

// ---------------------------------------------------------------------------
// Actor / Level
// ---------------------------------------------------------------------------
// Actor::level. Not independently verifiable from a static image; only used on
// the ClientInstance path, which validates the Level it produces by finding a
// structurally valid player map inside it before trusting anything.
constexpr std::ptrdiff_t ActorLevelOffset = 0x260;

// Level::getPlayerMap, newest-first, verbatim from Flarial dll-oss's
// maintained table (src/Utils/Memory/Game/Offset/OffsetInit.cpp). Their
// VersionUtils::addData() applies the per-version init functions oldest-first
// and stops at the running build, so the value in force for a 1.26.X client is
// the newest override at or below it: OffsetInit::init21130's 0x4E0 (init260
// does not touch this offset). 0x25F0 and 0x2608 are the much older 1.20.4 and
// 1.20.3 baselines and are kept only as last-resort probe candidates.
//
// The field at Level + offset is a *pointer* to the map on 1.21.40+, not an
// inline map (see Level::getPlayerMap's member_at<...*> branch).
//
// Which of these is correct for a given build is decided at runtime by
// PlausiblePlayerMapShape plus a full circular-list walk, never by assumption.
constexpr std::ptrdiff_t PlayerMapOffsetCandidates[] = {0x4E0, 0x4E8, 0x4F8, 0x25F0, 0x2608};

// ---------------------------------------------------------------------------
// Pure validation and selection
// ---------------------------------------------------------------------------

enum class ImageVerdict {
    Ok,
    NotRgba,
    UnsupportedExtent,
    ByteCountMismatch,
    NoPixels,
};

[[nodiscard]] constexpr const char* Describe(ImageVerdict verdict) noexcept {
    switch (verdict) {
        case ImageVerdict::Ok: return "ok";
        case ImageVerdict::NotRgba: return "format_not_rgba8";
        case ImageVerdict::UnsupportedExtent: return "unsupported_extent";
        case ImageVerdict::ByteCountMismatch: return "blob_size_mismatch";
        case ImageVerdict::NoPixels: return "null_pixels";
    }
    return "unknown";
}

// The classic Minecraft skin atlas is square and either 64x64 or 128x128 (HD).
// Persona skins are baked down to the same atlas whenever raw pixels exist at
// all, so this gate accepts them too - being persona is not disqualifying.
[[nodiscard]] constexpr bool IsSupportedSkinExtent(std::uint32_t width, std::uint32_t height) noexcept {
    return (width == 64 && height == 64) || (width == 128 && height == 128);
}

[[nodiscard]] constexpr std::uint64_t ExpectedByteCount(std::uint32_t width, std::uint32_t height) noexcept {
    return static_cast<std::uint64_t>(width) * height * 4;
}

[[nodiscard]] constexpr bool IsRgbaSkinFormat(std::int32_t format) noexcept {
    return format == RgbaUnormFormat || format == RgbaUnormSrgbFormat;
}

[[nodiscard]] constexpr ImageVerdict ClassifySkinImage(std::int32_t format, std::uint32_t width,
    std::uint32_t height, std::uint64_t byteCount, bool pixelsPresent) noexcept {
    if (!IsRgbaSkinFormat(format)) return ImageVerdict::NotRgba;
    if (!IsSupportedSkinExtent(width, height)) return ImageVerdict::UnsupportedExtent;
    if (byteCount != ExpectedByteCount(width, height)) return ImageVerdict::ByteCountMismatch;
    if (!pixelsPresent) return ImageVerdict::NoPixels;
    return ImageVerdict::Ok;
}

// A release std::string is self-consistent in a way random memory is not:
// capacity is either exactly the SSO capacity or a heap capacity that is at
// least the size, and Bedrock's skin/player identifiers are never huge.
[[nodiscard]] constexpr bool PlausibleStringFields(std::uint64_t size, std::uint64_t capacity,
    std::uint64_t sizeLimit = 1024) noexcept {
    if (capacity < size) return false;
    if (size > sizeLimit) return false;
    if (capacity == StringSsoCapacity) return true;
    return capacity > StringSsoCapacity && capacity <= sizeLimit * 4;
}

// PlayerSkin's trailing flag block is almost entirely bools. Requiring all of
// them to hold 0/1 (and TrustedSkinFlag/ArmSize::Type to hold a declared
// enumerator) is what makes a heap-wide search for PlayerSkin objects specific
// instead of a guess.
[[nodiscard]] constexpr bool PlausibleSkinFlags(std::uint8_t trusted, std::uint8_t premium,
    std::uint8_t persona, std::uint8_t personaCapeOnClassic, std::uint8_t primaryUser,
    std::uint8_t overridesAppearance, std::uint8_t armSize) noexcept {
    if (trusted > 2) return false;             // TrustedSkinFlag: Unset/False/True
    if (armSize > 3) return false;             // persona::ArmSize::Type: Slim..Unknown
    return premium <= 1 && persona <= 1 && personaCapeOnClassic <= 1
        && primaryUser <= 1 && overridesAppearance <= 1;
}

// Structural check on a candidate std::unordered_map, using only the scalar
// fields. A misread offset essentially never satisfies all of these at once:
// the bucket vector has to be a well-formed range of whole bucket pairs whose
// length agrees with _Mask, and the element count has to be sane.
[[nodiscard]] constexpr bool PlausiblePlayerMapShape(std::uint64_t size, std::uint64_t head,
    std::uint64_t bucketFirst, std::uint64_t bucketLast, std::uint64_t bucketEnd,
    std::uint64_t mask) noexcept {
    if (head == 0) return false;
    if (size == 0 || size > MaxPlayerMapEntries) return false;
    if (bucketFirst == 0 || bucketFirst > bucketLast || bucketLast > bucketEnd) return false;

    const std::uint64_t span = bucketLast - bucketFirst;
    // One bucket is a pair of list iterators, so 16 bytes.
    if (span == 0 || span % 16 != 0) return false;
    const std::uint64_t buckets = span / 16;
    // MSVC keeps _Mask == buckets - 1 with a power-of-two bucket count.
    if (mask + 1 != buckets) return false;
    return (buckets & (buckets - 1)) == 0;
}

// One candidate equipped-skin image found by either acquisition path.
struct SkinCandidate {
    std::uint64_t skinAddress{};
    std::uint64_t pixelsAddress{};
    std::uint32_t width{};
    std::uint32_t height{};
    bool primaryUser{};
    bool persona{};
    bool fromPlayerMap{};
    std::int32_t imageFormat{};
};

enum class UpdateSkinSignatureVariant {
    Missing,
    Current,
    Legacy,
    Ambiguous,
};

[[nodiscard]] constexpr bool IsLocalPlayerTypeName(std::string_view name) noexcept {
    const bool local = name.find("LocalPlayer") != std::string_view::npos;
    return local && name.find("Remote") == std::string_view::npos
        && name.find("ServerPlayer") == std::string_view::npos;
}

enum class RetainedPointerKind : std::uint32_t {
    None,
    ThreadOwner,
    PlayerSkin,
};

enum class RetainedOffsetStatus {
    Missing,
    Unique,
    Ambiguous,
};

struct RetainedPointerIdentity {
    std::uintptr_t owner{};
    std::uintptr_t skin{};
};

struct RetainedOffsetSelection {
    RetainedOffsetStatus status{RetainedOffsetStatus::Missing};
    std::size_t byteOffset{};
    RetainedPointerKind kind{RetainedPointerKind::None};
};

// Selects a Player member only when the same pointer slot follows A -> B -> A.
// The caller proves the A/B/A identity with pixel hashes; Bedrock is allowed
// to allocate a fresh ThreadOwner or PlayerSkin when returning to A.
// Direct PlayerSkin and ThreadOwner pointers are accepted; control-block-only
// matches are intentionally excluded because they cannot recover the atlas.
[[nodiscard]] constexpr RetainedOffsetSelection SelectRetainedSkinOffset(
    std::span<const std::uintptr_t> first,
    std::span<const std::uintptr_t> second,
    std::span<const std::uintptr_t> third,
    RetainedPointerIdentity firstIdentity,
    RetainedPointerIdentity secondIdentity,
    RetainedPointerIdentity thirdIdentity) noexcept {
    if (first.size() != second.size() || first.size() != third.size()
        || firstIdentity.skin == 0 || secondIdentity.skin == 0 || thirdIdentity.skin == 0) return {};

    RetainedOffsetSelection result{};
    for (std::size_t index = 0; index < first.size(); ++index) {
        RetainedPointerKind kind = RetainedPointerKind::None;
        if (first[index] == firstIdentity.owner && second[index] == secondIdentity.owner
            && third[index] == thirdIdentity.owner && firstIdentity.owner != 0
            && secondIdentity.owner != 0 && thirdIdentity.owner != 0) {
            kind = RetainedPointerKind::ThreadOwner;
        } else if (first[index] == firstIdentity.skin && second[index] == secondIdentity.skin
            && third[index] == thirdIdentity.skin) {
            kind = RetainedPointerKind::PlayerSkin;
        }
        if (kind == RetainedPointerKind::None) continue;
        if (result.status != RetainedOffsetStatus::Missing) {
            return {RetainedOffsetStatus::Ambiguous, 0, RetainedPointerKind::None};
        }
        result = {RetainedOffsetStatus::Unique, index * sizeof(std::uintptr_t), kind};
    }
    return result;
}

struct UpdateSkinSignatureSelection {
    UpdateSkinSignatureVariant variant{UpdateSkinSignatureVariant::Missing};
    std::uintptr_t address{};
};

// Signature variants are deliberately selected as one combined candidate set.
// Even if two patterns happen to resolve to the same address, accepting that
// overlap would make the compatibility decision dependent on pattern order.
[[nodiscard]] constexpr UpdateSkinSignatureSelection SelectUpdateSkinSignature(
    std::span<const std::uintptr_t> currentMatches,
    std::span<const std::uintptr_t> legacyMatches) noexcept {
    const auto total = currentMatches.size() + legacyMatches.size();
    if (total == 0) return {};
    if (total != 1) return {UpdateSkinSignatureVariant::Ambiguous, 0};
    if (!currentMatches.empty()) return {UpdateSkinSignatureVariant::Current, currentMatches.front()};
    return {UpdateSkinSignatureVariant::Legacy, legacyMatches.front()};
}

[[nodiscard]] constexpr bool IsPrimaryUpdateCandidate(const SkinCandidate& candidate) noexcept {
    return candidate.primaryUser && IsSupportedSkinExtent(candidate.width, candidate.height);
}

constexpr std::size_t NoCandidate = static_cast<std::size_t>(-1);

// Deterministic, documented ranking - the old reader took whatever matched
// first, which on a populated server is simply "some other player's skin".
//
//   1. mIsPrimaryUser wins. Bedrock sets it on the local account's own entry,
//      which is precisely the thing being rendered.
//   2. Entries reached through the authoritative player map beat heap finds.
//   3. A classic atlas beats a persona one when both exist for the same
//      player; a persona skin still qualifies when it is all there is.
//   4. Lowest address, purely so repeated scans agree with each other.
[[nodiscard]] constexpr std::size_t ChooseSkinCandidate(std::span<const SkinCandidate> candidates) noexcept {
    std::size_t best = NoCandidate;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto& candidate = candidates[index];
        if (!IsSupportedSkinExtent(candidate.width, candidate.height)) continue;
        if (best == NoCandidate) { best = index; continue; }
        const auto& incumbent = candidates[best];
        if (candidate.primaryUser != incumbent.primaryUser) {
            if (candidate.primaryUser) best = index;
            continue;
        }
        if (candidate.fromPlayerMap != incumbent.fromPlayerMap) {
            if (candidate.fromPlayerMap) best = index;
            continue;
        }
        if (candidate.persona != incumbent.persona) {
            if (!candidate.persona) best = index;
            continue;
        }
        if (candidate.skinAddress < incumbent.skinAddress) best = index;
    }
    return best;
}

// FNV-1a over the delivered pixels. Used only to avoid re-uploading an
// unchanged skin, so collisions cost a missed refresh, never correctness.
[[nodiscard]] constexpr std::uint64_t HashPixels(std::span<const std::uint8_t> pixels) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto value : pixels) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

// A structurally valid mce::Image is not necessarily a player skin. Texture
// caches and scratch buffers frequently have the same 64x64 RGBA shape. A
// real classic/HD skin must contain an opaque base-layer face at atlas
// coordinates (8,8)..(15,15), scaled for 128x128. This deliberately ignores
// the hat/overlay layer, which is allowed to be fully transparent.
[[nodiscard]] constexpr bool HasUsableHeadPixels(std::span<const std::uint8_t> pixels,
    std::uint32_t width, std::uint32_t height) noexcept {
    if (!IsSupportedSkinExtent(width, height)
        || pixels.size() != ExpectedByteCount(width, height)) return false;

    const std::uint32_t scale = width / 64;
    const std::uint32_t faceStart = 8 * scale;
    const std::uint32_t faceSize = 8 * scale;
    std::size_t opaqueFacePixels{};
    for (std::uint32_t y = faceStart; y < faceStart + faceSize; ++y) {
        for (std::uint32_t x = faceStart; x < faceStart + faceSize; ++x) {
            const auto alpha = pixels[(static_cast<std::size_t>(y) * width + x) * 4 + 3];
            if (alpha >= 128) ++opaqueFacePixels;
        }
    }
    // Permit deliberately cut-out faces while rejecting the nearly empty
    // scratch atlas that was previously cached as the equipped skin.
    return opaqueFacePixels >= static_cast<std::size_t>(faceSize) * faceSize / 4;
}

}
