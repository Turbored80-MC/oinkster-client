#include "Oinkster/Json.hpp"
#include <charconv>
#include <cmath>
#include <format>

namespace Oinkster::Json {
namespace {
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Value> Document() {
        SkipWhitespace();
        auto value = ParseValue(0);
        if (!value) return std::nullopt;
        SkipWhitespace();
        if (position_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    void SkipWhitespace() noexcept {
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (character != ' ' && character != '\t' && character != '\n' && character != '\r') break;
            ++position_;
        }
    }

    bool Consume(std::string_view literal) noexcept {
        if (text_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        return true;
    }

    std::optional<Value> ParseValue(std::size_t depth) {
        if (depth > MaxDepth || position_ >= text_.size()) return std::nullopt;
        switch (text_[position_]) {
        case '{': return ParseObject(depth);
        case '[': return ParseArray(depth);
        case '"': {
            auto text = ParseString();
            if (!text) return std::nullopt;
            return Value{std::move(*text)};
        }
        case 't': return Consume("true") ? std::optional(Value{true}) : std::nullopt;
        case 'f': return Consume("false") ? std::optional(Value{false}) : std::nullopt;
        case 'n': return Consume("null") ? std::optional(Value{nullptr}) : std::nullopt;
        default: return ParseNumber();
        }
    }

    std::optional<Value> ParseNumber() {
        const std::size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        const auto digits = [&] {
            const std::size_t first = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            return position_ - first;
        };
        const std::size_t integer = digits();
        if (integer == 0 || (integer > 1 && text_[position_ - integer] == '0')) return std::nullopt;
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            if (digits() == 0) return std::nullopt;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            if (digits() == 0) return std::nullopt;
        }
        double value{};
        const auto result = std::from_chars(text_.data() + start, text_.data() + position_, value);
        if (result.ec != std::errc{} || !std::isfinite(value)) return std::nullopt;
        return Value{value};
    }

    static void AppendUtf8(std::string& output, std::uint32_t codePoint) {
        if (codePoint < 0x80) output.push_back(static_cast<char>(codePoint));
        else if (codePoint < 0x800) {
            output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint < 0x10000) {
            output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    std::optional<std::uint32_t> ParseHex4() {
        if (position_ + 4 > text_.size()) return std::nullopt;
        std::uint32_t value{};
        const auto result = std::from_chars(text_.data() + position_, text_.data() + position_ + 4, value, 16);
        if (result.ec != std::errc{} || result.ptr != text_.data() + position_ + 4) return std::nullopt;
        position_ += 4;
        return value;
    }

    std::optional<std::string> ParseString() {
        if (!Consume("\"")) return std::nullopt;
        std::string output;
        while (position_ < text_.size()) {
            const char character = text_[position_++];
            if (character == '"') return output;
            if (static_cast<unsigned char>(character) < 0x20) return std::nullopt;
            if (character != '\\') { output.push_back(character); continue; }
            if (position_ >= text_.size()) return std::nullopt;
            switch (text_[position_++]) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                auto unit = ParseHex4();
                if (!unit) return std::nullopt;
                std::uint32_t codePoint = *unit;
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    if (!Consume("\\u")) return std::nullopt;
                    const auto low = ParseHex4();
                    if (!low || *low < 0xDC00 || *low > 0xDFFF) return std::nullopt;
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (*low - 0xDC00);
                } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                    return std::nullopt;
                }
                AppendUtf8(output, codePoint);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Value> ParseArray(std::size_t depth) {
        ++position_;
        auto array = std::make_shared<Array>();
        SkipWhitespace();
        if (Consume("]")) return Value{std::move(array)};
        while (true) {
            SkipWhitespace();
            auto value = ParseValue(depth + 1);
            if (!value || array->size() >= MaxContainerEntries) return std::nullopt;
            array->push_back(std::move(*value));
            SkipWhitespace();
            if (Consume("]")) return Value{std::move(array)};
            if (!Consume(",")) return std::nullopt;
        }
    }

    std::optional<Value> ParseObject(std::size_t depth) {
        ++position_;
        auto object = std::make_shared<Object>();
        SkipWhitespace();
        if (Consume("}")) return Value{std::move(object)};
        while (true) {
            SkipWhitespace();
            auto key = ParseString();
            if (!key) return std::nullopt;
            SkipWhitespace();
            if (!Consume(":")) return std::nullopt;
            SkipWhitespace();
            auto value = ParseValue(depth + 1);
            if (!value || object->size() >= MaxContainerEntries) return std::nullopt;
            (*object)[std::move(*key)] = std::move(*value);
            SkipWhitespace();
            if (Consume("}")) return Value{std::move(object)};
            if (!Consume(",")) return std::nullopt;
        }
    }

    std::string_view text_;
    std::size_t position_{};
};
}

const Value* Value::Find(std::string_view key) const noexcept {
    const auto* object = AsObject();
    if (!object) return nullptr;
    const auto found = object->find(key);
    return found == object->end() ? nullptr : &found->second;
}

std::optional<Value> Parse(std::string_view text) {
    if (text.size() > MaxDocumentBytes) return std::nullopt;
    // Tolerate a UTF-8 byte-order mark (Notepad adds one).
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    try {
        return Parser(text).Document();
    } catch (...) {
        return std::nullopt;
    }
}

std::string Escape(std::string_view text) {
    std::string output;
    output.reserve(text.size() + 2);
    for (const char character : text) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) output += std::format("\\u{:04x}", static_cast<unsigned>(character));
            else output.push_back(character);
        }
    }
    return output;
}

}
