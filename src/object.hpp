#pragma once
#include <format>
#include <memory>
#include <string>
#include <variant>
#include <unordered_map>
#include <vector>

#include "utility/print.hpp"
#include "utility/value_ptr.hpp"

namespace anzu {

// Want these to be equivalent since we want uints available in the runtime but we also want
// to use it as indexes into C++ vectors which use size_t.
static_assert(std::is_same_v<std::uint64_t, std::size_t>);

struct type_name;

struct type_simple
{
    std::string name;
    auto operator==(const type_simple&) const -> bool = default;
};

struct type_list
{
    value_ptr<type_name> inner_type;
    std::size_t          count;
    auto operator==(const type_list&) const -> bool = default;
};

struct type_ptr
{
    value_ptr<type_name> inner_type;
    auto operator==(const type_ptr&) const -> bool = default;
};

struct type_span
{
    value_ptr<type_name> inner_type;
    auto operator==(const type_span&) const -> bool = default;
};

struct type_name : public std::variant<
    type_simple,
    type_list,
    type_ptr,
    type_span>
{
    using variant::variant;
};

using type_names = std::vector<type_name>;

struct object
{
    std::vector<std::byte> data;
    anzu::type_name        type;
};

template <typename T>
inline auto as_bytes(const T& val) -> std::array<std::byte, sizeof(T)>
{
    return std::bit_cast<std::array<std::byte, sizeof(T)>>(val);
}

struct field
{
    std::string name;
    type_name   type;
    auto operator==(const field&) const -> bool = default;
};
using type_fields = std::vector<field>;

struct type_info
{
    type_fields fields;
};

auto hash(const type_name& type) -> std::size_t;
auto hash(const type_list& type) -> std::size_t;
auto hash(const type_ptr& type) -> std::size_t;
auto hash(const type_span& type) -> std::size_t;
auto hash(const type_simple& type) -> std::size_t;

auto i32_type() -> type_name;
auto i64_type() -> type_name;
auto u64_type() -> type_name;
auto f64_type() -> type_name;
auto char_type() -> type_name;
auto bool_type() -> type_name;
auto null_type() -> type_name;

auto make_type(const std::string& name) -> type_name;

auto concrete_list_type(const type_name& t, std::size_t size) -> type_name;
auto is_list_type(const type_name& t) -> bool;

auto concrete_ptr_type(const type_name& t) -> type_name;
auto is_ptr_type(const type_name& t) -> bool;

auto concrete_span_type(const type_name& t) -> type_name;
auto is_span_type(const type_name& t) -> bool;

auto size_of_ptr() -> std::size_t;

// Extracts the single inner type of the given t. Undefined if the given t is not a compound
// type with a single subtype.
auto inner_type(const type_name& t) -> type_name;

// Extracts the array size of the given type. Undefined if the given t is not an array
auto array_length(const type_name& t) -> std::size_t;

auto is_type_fundamental(const type_name& type) -> bool;

auto is_type_trivially_copyable(const type_name& type) -> bool;

class type_store
{
    using type_hash = decltype([](const type_name& t) { return anzu::hash(t); });
    std::unordered_map<type_name, type_info, type_hash> d_classes;

public:
    auto add(const type_name& name, const type_fields& fields) -> bool;
    auto contains(const type_name& t) const -> bool;

    auto size_of(const type_name& t) const -> std::size_t;
    auto fields_of(const type_name& t) const -> type_fields;
};

auto to_string(const object& object) -> std::string;
auto to_string(const type_name& type) -> std::string;
auto to_string(const type_list& type) -> std::string;
auto to_string(const type_ptr& type) -> std::string;
auto to_string(const type_span& type) -> std::string;
auto to_string(const type_simple& type) -> std::string;

}

template <> struct std::formatter<std::byte> : std::formatter<std::string> {
    auto format(std::byte b, auto& ctx) {
        const auto str = std::format("{:X}", static_cast<unsigned char>(b));
        return std::formatter<std::string>::format(str, ctx);
    }
};

template <> struct std::formatter<anzu::object> : std::formatter<std::string> {
    auto format(const anzu::object& obj, auto& ctx) {
        return std::formatter<std::string>::format(to_string(obj), ctx);
    }
};

template <> struct std::formatter<anzu::type_name> : std::formatter<std::string>
{
    auto format(const anzu::type_name& type, auto& ctx) {
        return std::formatter<std::string>::format(anzu::to_string(type), ctx);
    }
};