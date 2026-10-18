#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Servers define their own blocks (The Hive's "hive:mossy_dirt", ...) in
// StartGame's block-properties list. A client joining the server registers
// them from there; the replay world, which the game hosts itself, does not,
// so recorded chunks show those blocks as "update!" placeholders. This turns
// the recorded definitions back into a behavior pack the replay world loads
// at startup. Their models and textures come from the server's resource
// packs, which playback already applies. Pure logic; ReplayWorld writes it.
namespace Oinkster::ReplayBlockPack {

// A network-NBT value (varint lengths and ints, as StartGame carries it).
struct Nbt {
    enum class Kind : std::uint8_t { Byte, Short, Int, Long, Float, Double, String, List, Compound, ByteArray, IntArray, LongArray };
    Kind kind{Kind::Compound};
    double number{};
    std::string text;
    std::vector<Nbt> items;
    std::vector<std::pair<std::string, Nbt>> fields;

    [[nodiscard]] const Nbt* Find(std::string_view name) const noexcept;
};

struct CustomBlock {
    std::string name;
    Nbt definition;
};

// The custom-block definitions in a StartGame packet (header included), in
// the order the server listed them. Empty when there are none or the list
// cannot be read.
[[nodiscard]] std::vector<CustomBlock> FindCustomBlocks(std::span<const std::uint8_t> startGame);

// One block as a behavior-pack block file (format 1.21.40).
[[nodiscard]] std::string BlockJson(const CustomBlock& block);

struct PackFile {
    std::string path;  // relative to the pack folder, forward slashes
    std::string text;
};

inline constexpr std::string_view PackFolderName = "oinkster_replay_blocks";
inline constexpr std::string_view PackUuid = "6f696e6b-7374-6572-2d62-6c6f636b7301";

// The manifest and one file per block.
[[nodiscard]] std::vector<PackFile> BuildPack(std::span<const CustomBlock> blocks);
// world_behavior_packs.json naming the pack.
[[nodiscard]] std::string WorldPacksJson();

}
