#include "Oinkster/ReplayBlockPack.hpp"

#include "Oinkster/RakNet.hpp"
#include "Oinkster/ReplayCapture.hpp"
#include <algorithm>
#include <bit>
#include <format>

namespace Oinkster::ReplayBlockPack {
namespace {

constexpr int MaxDepth = 24;
constexpr std::int64_t MaxElements = 65536;

std::int64_t ZigZag(std::uint64_t value) noexcept {
    return static_cast<std::int64_t>(value >> 1U) ^ -static_cast<std::int64_t>(value & 1U);
}

std::optional<Nbt> ReadValue(RakNet::Reader& reader, std::uint8_t tag, int depth);

std::optional<Nbt> ReadCompound(RakNet::Reader& reader, int depth) {
    Nbt compound;
    compound.kind = Nbt::Kind::Compound;
    for (;;) {
        const auto tag = reader.U8();
        if (!reader.Ok()) return std::nullopt;
        if (tag == 0) return compound;
        const auto name = reader.Bytes(reader.VarU32());
        if (!reader.Ok()) return std::nullopt;
        auto value = ReadValue(reader, tag, depth + 1);
        if (!value) return std::nullopt;
        compound.fields.emplace_back(std::string(name.begin(), name.end()), std::move(*value));
    }
}

std::optional<Nbt> ReadValue(RakNet::Reader& reader, std::uint8_t tag, int depth) {
    if (depth > MaxDepth) return std::nullopt;
    Nbt value;
    const auto count = [&]() -> std::optional<std::int64_t> {
        const auto length = ZigZag(reader.VarU32());
        if (!reader.Ok() || length < 0 || length > MaxElements) return std::nullopt;
        return length;
    };
    switch (tag) {
    case 1:
        value.kind = Nbt::Kind::Byte;
        value.number = reader.U8();
        break;
    case 2:
        value.kind = Nbt::Kind::Short;
        value.number = static_cast<std::int16_t>(reader.U16LE());
        break;
    case 3:
        value.kind = Nbt::Kind::Int;
        value.number = static_cast<double>(ZigZag(reader.VarU32()));
        break;
    case 4:
        value.kind = Nbt::Kind::Long;
        value.number = static_cast<double>(ZigZag(reader.VarU64()));
        break;
    case 5: {
        value.kind = Nbt::Kind::Float;
        const auto bytes = reader.Bytes(4);
        if (bytes.size() != 4) return std::nullopt;
        std::uint32_t bits{};
        for (int index = 3; index >= 0; --index) bits = (bits << 8U) | bytes[static_cast<std::size_t>(index)];
        value.number = std::bit_cast<float>(bits);
        break;
    }
    case 6: {
        value.kind = Nbt::Kind::Double;
        const auto bytes = reader.Bytes(8);
        if (bytes.size() != 8) return std::nullopt;
        std::uint64_t bits{};
        for (int index = 7; index >= 0; --index) bits = (bits << 8U) | bytes[static_cast<std::size_t>(index)];
        value.number = std::bit_cast<double>(bits);
        break;
    }
    case 7: {
        value.kind = Nbt::Kind::ByteArray;
        const auto length = count();
        if (!length) return std::nullopt;
        const auto bytes = reader.Bytes(static_cast<std::size_t>(*length));
        for (const auto byte : bytes) {
            Nbt element;
            element.kind = Nbt::Kind::Byte;
            element.number = byte;
            value.items.push_back(element);
        }
        break;
    }
    case 8: {
        value.kind = Nbt::Kind::String;
        const auto text = reader.Bytes(reader.VarU32());
        value.text.assign(text.begin(), text.end());
        break;
    }
    case 9: {
        value.kind = Nbt::Kind::List;
        const auto element = reader.U8();
        const auto length = count();
        if (!length) return std::nullopt;
        for (std::int64_t index = 0; index < *length; ++index) {
            auto item = ReadValue(reader, element, depth + 1);
            if (!item) return std::nullopt;
            value.items.push_back(std::move(*item));
        }
        break;
    }
    case 10: {
        auto compound = ReadCompound(reader, depth);
        if (!compound) return std::nullopt;
        value = std::move(*compound);
        break;
    }
    case 11: case 12: {
        value.kind = tag == 11 ? Nbt::Kind::IntArray : Nbt::Kind::LongArray;
        const auto length = count();
        if (!length) return std::nullopt;
        for (std::int64_t index = 0; index < *length; ++index) {
            Nbt element;
            element.kind = tag == 11 ? Nbt::Kind::Int : Nbt::Kind::Long;
            element.number = static_cast<double>(ZigZag(reader.VarU64()));
            value.items.push_back(element);
        }
        break;
    }
    default:
        return std::nullopt;
    }
    if (!reader.Ok()) return std::nullopt;
    return value;
}

bool IdentifierChar(std::uint8_t c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' || c == '/';
}

// "namespace:name", lower-case, as block identifiers are.
bool PlausibleIdentifier(std::span<const std::uint8_t> name) noexcept {
    if (name.size() < 3 || name.size() > 128) return false;
    const auto colon = std::ranges::find(name, std::uint8_t{':'});
    if (colon == name.begin() || colon == name.end() || colon + 1 == name.end()) return false;
    return std::all_of(name.begin(), colon, IdentifierChar) && std::all_of(colon + 1, name.end(), IdentifierChar);
}

// One list entry at the reader: the identifier, then the definition as an
// unnamed root compound (tag 10, empty name).
std::optional<CustomBlock> ReadEntry(RakNet::Reader& reader) {
    const auto name = reader.Bytes(reader.VarU32());
    if (!reader.Ok() || !PlausibleIdentifier(name)) return std::nullopt;
    if (reader.U8() != 10 || reader.VarU32() != 0 || !reader.Ok()) return std::nullopt;
    auto definition = ReadCompound(reader, 0);
    if (!definition || !definition->Find("components")) return std::nullopt;
    return CustomBlock{std::string(name.begin(), name.end()), std::move(*definition)};
}

// --- JSON -------------------------------------------------------------------

std::string Quoted(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (byte < 0x20) {
            out += std::format("\\u{:04x}", byte);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::string Number(const Nbt& value) {
    if (value.kind == Nbt::Kind::Float) return std::format("{}", static_cast<float>(value.number));
    if (value.kind == Nbt::Kind::Double) return std::format("{}", value.number);
    return std::format("{}", static_cast<std::int64_t>(value.number));
}

double NumberOf(const Nbt* value, double fallback = 0.0) {
    if (!value || value->kind == Nbt::Kind::String || value->kind == Nbt::Kind::Compound || value->kind == Nbt::Kind::List) return fallback;
    return value->number;
}

std::string TextOf(const Nbt* value) {
    return value && value->kind == Nbt::Kind::String ? value->text : std::string();
}

std::string Vector3(const Nbt* list) {
    if (!list || list->items.size() != 3) return {};
    return std::format("[{}, {}, {}]", Number(list->items[0]), Number(list->items[1]), Number(list->items[2]));
}

// A collision or selection box: off, or one box by origin and size.
std::string Box(const Nbt& box) {
    if (NumberOf(box.Find("enabled"), 1.0) == 0.0) return "false";
    const auto origin = Vector3(box.Find("origin"));
    const auto size = Vector3(box.Find("size"));
    if (origin.empty() || size.empty()) return "true";
    return std::format("{{\"origin\": {}, \"size\": {}}}", origin, size);
}

std::string Material(const Nbt& material) {
    std::string out = "{";
    out += "\"texture\": " + Quoted(TextOf(material.Find("texture")));
    if (const auto method = TextOf(material.Find("render_method")); !method.empty()) out += ", \"render_method\": " + Quoted(method);
    if (const auto* occlusion = material.Find("ambient_occlusion")) {
        out += std::format(", \"ambient_occlusion\": {}", NumberOf(occlusion) != 0.0);
    }
    if (const auto* dimming = material.Find("face_dimming")) out += std::format(", \"face_dimming\": {}", NumberOf(dimming) != 0.0);
    return out + "}";
}

std::string MaterialInstances(const Nbt& instances) {
    std::vector<std::string> entries;
    if (const auto* materials = instances.Find("materials")) {
        for (const auto& [face, material] : materials->fields) entries.push_back(Quoted(face) + ": " + Material(material));
    }
    if (const auto* mappings = instances.Find("mappings")) {
        for (const auto& [face, target] : mappings->fields) {
            if (target.kind == Nbt::Kind::String) entries.push_back(Quoted(face) + ": " + Quoted(target.text));
        }
    }
    std::string out = "{";
    for (std::size_t index = 0; index < entries.size(); ++index) out += (index ? ", " : "") + entries[index];
    return out + "}";
}

// Quarter turns, as the network form stores rotation, back to degrees.
std::string Transformation(const Nbt& transform) {
    const auto component = [&](const char* name, double fallback) { return NumberOf(transform.Find(name), fallback); };
    return std::format("{{\"rotation\": [{}, {}, {}], \"translation\": [{}, {}, {}], \"scale\": [{}, {}, {}]}}",
        component("RX", 0) * 90.0, component("RY", 0) * 90.0, component("RZ", 0) * 90.0,
        component("TX", 0), component("TY", 0), component("TZ", 0),
        component("SX", 1), component("SY", 1), component("SZ", 1));
}

// The components a replay needs to draw and collide with the block; events
// and creative-menu data are left out.
std::string Components(const Nbt& components) {
    std::vector<std::string> out;
    for (const auto& [name, value] : components.fields) {
        if (name == "minecraft:geometry") {
            if (const auto identifier = TextOf(value.Find("identifier")); !identifier.empty()) {
                out.push_back(std::format("\"minecraft:geometry\": {{\"identifier\": {}}}", Quoted(identifier)));
            }
        } else if (name == "minecraft:material_instances") {
            out.push_back("\"minecraft:material_instances\": " + MaterialInstances(value));
        } else if (name == "minecraft:collision_box" || name == "minecraft:selection_box") {
            out.push_back(Quoted(name) + ": " + Box(value));
        } else if (name == "minecraft:transformation") {
            out.push_back("\"minecraft:transformation\": " + Transformation(value));
        } else if (name == "minecraft:friction") {
            out.push_back(std::format("\"minecraft:friction\": {}", static_cast<float>(std::clamp(NumberOf(value.Find("value"), 0.4), 0.0, 0.9))));
        } else if (name == "minecraft:destructible_by_mining") {
            out.push_back(std::format("\"minecraft:destructible_by_mining\": {{\"seconds_to_destroy\": {}}}",
                static_cast<float>(std::max(0.0, NumberOf(value.Find("value"))))));
        } else if (name == "minecraft:block_light_filter") {
            out.push_back(std::format("\"minecraft:light_dampening\": {}",
                std::clamp(static_cast<int>(NumberOf(value.Find("lightLevel"), 15)), 0, 15)));
        } else if (name == "minecraft:light_emission") {
            out.push_back(std::format("\"minecraft:light_emission\": {}",
                std::clamp(static_cast<int>(NumberOf(value.Find("emission"))), 0, 15)));
        }
    }
    std::string text = "{";
    for (std::size_t index = 0; index < out.size(); ++index) text += (index ? ", " : "") + out[index];
    return text + "}";
}

std::string StateValue(const Nbt& value) {
    if (value.kind == Nbt::Kind::String) return Quoted(value.text);
    if (value.kind == Nbt::Kind::Byte) return value.number != 0.0 ? "true" : "false";
    return Number(value);
}

// Format 1.20.20+ reads states with query.block_state; the network form
// still says query.block_property.
std::string Condition(std::string condition) {
    constexpr std::string_view Old = "query.block_property(";
    constexpr std::string_view New = "query.block_state(";
    for (auto at = condition.find(Old); at != std::string::npos; at = condition.find(Old, at + New.size())) {
        condition.replace(at, Old.size(), New);
    }
    return condition;
}

}

const Nbt* Nbt::Find(std::string_view name) const noexcept {
    const auto found = std::ranges::find_if(fields, [&](const auto& field) { return field.first == name; });
    return found == fields.end() ? nullptr : &found->second;
}

std::vector<CustomBlock> FindCustomBlocks(std::span<const std::uint8_t> startGame) {
    const auto header = ReplayCapture::ParseHeader(startGame);
    if (!header) return {};
    const auto body = startGame.subspan(header->length);
    // The list sits deep inside StartGame, after fields whose layout changes
    // between versions: find its first entry (an identifier followed by a
    // compound with components), then read entries one after another.
    for (std::size_t at = 0; at + 4 < body.size(); ++at) {
        const auto length = body[at];
        if (length < 3 || length > 127 || at + 1 + length + 2 > body.size()) continue;
        const auto name = body.subspan(at + 1, length);
        if (body[at + 1 + length] != 10 || body[at + 2 + length] != 0 || !PlausibleIdentifier(name)) continue;
        RakNet::Reader reader(body.subspan(at));
        std::vector<CustomBlock> blocks;
        // Stops at the end of the list (or when this was not a block at all).
        while (reader.Remaining() > 0 && blocks.size() < 8192) {
            auto entry = ReadEntry(reader);
            if (!entry) break;
            blocks.push_back(std::move(*entry));
        }
        if (!blocks.empty()) return blocks;
    }
    return {};
}

std::string BlockJson(const CustomBlock& block) {
    std::string description = "\"identifier\": " + Quoted(block.name);
    if (const auto* menu = block.definition.Find("menu_category")) {
        auto category = TextOf(menu->Find("category"));
        if (category.empty()) category = "none";
        description += ", \"menu_category\": {\"category\": " + Quoted(category);
        if (const auto group = TextOf(menu->Find("group")); !group.empty()) description += ", \"group\": " + Quoted(group);
        description += "}";
    }
    if (const auto* properties = block.definition.Find("properties"); properties && !properties->items.empty()) {
        std::vector<std::string> states;
        for (const auto& property : properties->items) {
            const auto name = TextOf(property.Find("name"));
            const auto* values = property.Find("enum");
            if (name.empty() || !values || values->items.empty()) continue;
            std::string list = "[";
            for (std::size_t index = 0; index < values->items.size(); ++index) {
                list += (index ? ", " : "") + StateValue(values->items[index]);
            }
            states.push_back(Quoted(name) + ": " + list + "]");
        }
        if (!states.empty()) {
            description += ", \"states\": {";
            for (std::size_t index = 0; index < states.size(); ++index) description += (index ? ", " : "") + states[index];
            description += "}";
        }
    }
    std::string json = "{\"format_version\": \"1.21.40\", \"minecraft:block\": {\"description\": {" + description + "}";
    if (const auto* components = block.definition.Find("components")) json += ", \"components\": " + Components(*components);
    if (const auto* permutations = block.definition.Find("permutations"); permutations && !permutations->items.empty()) {
        json += ", \"permutations\": [";
        bool first = true;
        for (const auto& permutation : permutations->items) {
            const auto condition = TextOf(permutation.Find("condition"));
            const auto* components = permutation.Find("components");
            if (condition.empty() || !components) continue;
            json += std::format("{}{{\"condition\": {}, \"components\": {}}}", first ? "" : ", ",
                Quoted(Condition(condition)), Components(*components));
            first = false;
        }
        json += "]";
    }
    return json + "}}";
}

std::vector<PackFile> BuildPack(std::span<const CustomBlock> blocks) {
    std::vector<PackFile> files;
    files.push_back({"manifest.json", std::format(
        "{{\"format_version\": 2, \"header\": {{\"name\": \"Oinkster replay blocks\", "
        "\"description\": \"Blocks the recorded server defined, for replay playback.\", \"uuid\": {}, "
        "\"version\": [1, 0, 0], \"min_engine_version\": [1, 21, 40]}}, "
        "\"modules\": [{{\"type\": \"data\", \"uuid\": \"6f696e6b-7374-6572-2d62-6c6f636b7302\", \"version\": [1, 0, 0]}}]}}",
        Quoted(PackUuid))});
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        files.push_back({std::format("blocks/block_{:04}.json", index), BlockJson(blocks[index])});
    }
    return files;
}

std::string WorldPacksJson() {
    return std::format("[{{\"pack_id\": {}, \"version\": [1, 0, 0]}}]", Quoted(PackUuid));
}

}
