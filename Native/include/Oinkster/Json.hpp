#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Minimal, bounded JSON reader for settings.json and the catalog parity
// test. Strict RFC 8259 syntax; depth, input size and container sizes are
// capped so a hostile or corrupt file cannot exhaust the stack or memory.
// Strings are returned as UTF-8 (\u escapes and surrogate pairs decoded).
namespace Oinkster::Json {

struct Value;
using Object = std::map<std::string, Value, std::less<>>;
using Array = std::vector<Value>;

struct Value {
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> data{nullptr};

    [[nodiscard]] bool IsNull() const noexcept { return std::holds_alternative<std::nullptr_t>(data); }
    [[nodiscard]] const bool* AsBool() const noexcept { return std::get_if<bool>(&data); }
    [[nodiscard]] const double* AsNumber() const noexcept { return std::get_if<double>(&data); }
    [[nodiscard]] const std::string* AsString() const noexcept { return std::get_if<std::string>(&data); }
    [[nodiscard]] const Array* AsArray() const noexcept {
        const auto* value = std::get_if<std::shared_ptr<Array>>(&data);
        return value ? value->get() : nullptr;
    }
    [[nodiscard]] const Object* AsObject() const noexcept {
        const auto* value = std::get_if<std::shared_ptr<Object>>(&data);
        return value ? value->get() : nullptr;
    }
    // Member lookup on an object; nullptr when absent or not an object.
    [[nodiscard]] const Value* Find(std::string_view key) const noexcept;
};

inline constexpr std::size_t MaxDocumentBytes = 4 * 1024 * 1024;
inline constexpr std::size_t MaxDepth = 32;
inline constexpr std::size_t MaxContainerEntries = 16384;

// nullopt on any syntax error, trailing garbage, or limit violation.
[[nodiscard]] std::optional<Value> Parse(std::string_view text);

// Escapes a UTF-8 string for embedding between JSON quotes.
[[nodiscard]] std::string Escape(std::string_view text);

}
