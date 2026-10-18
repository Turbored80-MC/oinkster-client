#pragma once

#include "Oinkster/Events.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The crosshair texture the game is drawing: textures/ui/cross_hair.png from
// the highest-priority active resource pack that has one, else vanilla.
//
// Packs are identified by the header uuid ResourcePackTelemetry reads from
// each active PackManifest and matched against the manifest.json of the pack
// folders on disk (the player's resource_packs, development_resource_packs
// and world packs, plus the game's own built-in packs). Files are read and
// decoded on a worker thread, never on the render thread. Encrypted packs
// (Marketplace, most server packs) have no readable PNG and are skipped.
namespace Oinkster::CrosshairTexture {

// Pure helpers (unit-tested).
//
// Bedrock .brarchive: 8-byte magic, u32 entry count, u32 version, then one
// 256-byte entry per file (u8 name length, the name, u32 offset and u32 size
// at +248) and the file data, offsets relative to the end of the table.
[[nodiscard]] std::optional<std::span<const std::uint8_t>> FindBrarchiveEntry(
    std::span<const std::uint8_t> archive, std::string_view name) noexcept;
// PackManifest's uuid as two 64-bit halves, formatted the way the game does.
[[nodiscard]] std::string FormatPackUuid(std::uint64_t high, std::uint64_t low);
// header.uuid of a manifest.json (comments tolerated), lowercased.
[[nodiscard]] std::optional<std::string> ManifestHeaderUuid(std::string_view manifest);
// PNG -> ARGB via WIC; nullopt for anything else or images over 512 px.
[[nodiscard]] std::optional<ArgbImage> DecodePng(std::span<const std::uint8_t> bytes) noexcept;

// Worker lifecycle. Request is cheap and may be called every few seconds
// from the render thread with the active packs, highest priority first;
// the texture is re-resolved only when that list changes.
void Start() noexcept;
void Stop() noexcept;
void Request(std::vector<std::string> packUuids) noexcept;
[[nodiscard]] std::shared_ptr<const ArgbImage> Current() noexcept;

}
