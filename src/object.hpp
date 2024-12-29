#pragma once
#include <format>
#include <memory>
#include <string>
#include <variant>
#include <unordered_map>
#include <vector>
#include <span>
#include <filesystem>
#include <utility>
#include <concepts>

#include "utility/common.hpp"
#include "utility/value_ptr.hpp"
#include "utility/hash.hpp"

namespace anzu {

// Want these to be equivalent since we want uints available in the runtime but we also want
// to use it as indexes into C++ vectors which use size_t.
static_assert(std::is_same_v<std::uint64_t, std::size_t>);
struct type_name;

struct type_null
{
    auto to_hash() const { return hash(0); }
    auto to_string() const -> std::string;
    auto operator==(const type_null&) const -> bool = default;
};

struct type_bool
{
    auto to_hash() const { return hash(0); }
    auto to_string() const -> std::string;
    auto operator==(const type_bool&) const -> bool = default;
};

struct type_char
{
    auto to_hash() const { return hash(0); }
    auto to_string() const -> std::string;
    auto operator==(const type_char&) const -> bool = default;
};

struct type_i32
{
    auto to_hash() const { return hash(0); }
    auto to_string() const -> std::string;
    auto operator==(const type_i32&) const -> bool = default;
};

struct type_i64
{
    auto to_hash() const { return hash(0); }
    auto to_string() const -> std::string;
    auto operator==(const type_i64&) const -> bool = default;
};

struct type_u64
{
    auto to_hash() const { return hash(0); }
    auto to_string() const -> std::string;
    auto operator==(const type_u64&) const -> bool = default;
};

struct type_f64
{
    auto to_hash() const { return hash(0); }
    auto to_string() const -> std::string;
    auto operator==(const type_f64&) const -> bool = default;
};

struct type_type
{
    auto to_hash() const { return hash(0); }
    auto to_string() const -> std::string;
    auto operator==(const type_type&) const -> bool = default;
};

struct type_arena
{
    auto to_hash() const { return hash(0); }
    auto to_string() const -> std::string;
    auto operator==(const type_arena&) const -> bool = default;
};


struct type_module
{
    auto to_hash() const { return hash(0); }
    auto to_string() const -> std::string;
    auto operator==(const type_module&) const -> bool = default;
};

struct type_struct
{
    std::string            name;
    std::filesystem::path  module;
    std::vector<type_name> templates;

    auto to_hash() const { return hash(name, module, templates); }
    auto to_string() const -> std::string;
    auto operator==(const type_struct&) const -> bool = default;
};

struct type_array
{
    value_ptr<type_name> inner_type;
    std::size_t          count;

    auto to_hash() const { return hash(inner_type, count); }
    auto to_string() const -> std::string;
    auto operator==(const type_array&) const -> bool = default;
};

struct type_ptr
{
    value_ptr<type_name> inner_type;

    auto to_hash() const { return hash(inner_type); }
    auto to_string() const -> std::string;
    auto operator==(const type_ptr&) const -> bool = default;
};

struct type_span
{
    value_ptr<type_name> inner_type;

    auto to_hash() const { return hash(inner_type); }
    auto to_string() const -> std::string;
    auto operator==(const type_span&) const -> bool = default;
};

struct type_optional
{
    value_ptr<type_name> inner_type;

    auto to_hash() const { return hash(inner_type); }
    auto to_string() const -> std::string;
    auto operator==(const type_optional&) const -> bool = default;
};

struct type_function_ptr
{
    std::vector<type_name> param_types;
    value_ptr<type_name>   return_type;

    auto to_hash() const { return hash(param_types, return_type); }
    auto to_string() const -> std::string;
    auto operator==(const type_function_ptr&) const -> bool = default;
};

struct type_bound_method
{
    std::size_t            id;
    std::vector<type_name> param_types;
    value_ptr<type_name>   return_type;

    auto to_hash() const { return hash(id, param_types, return_type); }
    auto to_string() const -> std::string;
    auto operator==(const type_bound_method&) const -> bool = default;
};

struct type_bound_method_template
{
    std::filesystem::path    module;
    type_struct              struct_name;
    std::string              name;

    auto to_hash() const { return hash(module, struct_name, name); }
    auto to_string() const -> std::string;
    auto operator==(const type_bound_method_template&) const -> bool = default;
};

struct type_function
{
    std::size_t            id;
    std::vector<type_name> param_types;
    value_ptr<type_name>   return_type;

    auto to_pointer() const -> type_name;
    auto to_hash() const { return hash(id, param_types, return_type); }
    auto to_bound_method() -> type_bound_method { return {id, param_types, return_type}; }
    auto to_string() const -> std::string;
    auto operator==(const type_function&) const -> bool = default;
};

struct type_function_template
{
    std::filesystem::path    module;
    type_struct              struct_name;
    std::string              name;

    auto to_hash() const { return hash(module, struct_name, name); }
    auto to_string() const -> std::string;
    auto operator==(const type_function_template&) const -> bool = default;
};

struct type_struct_template
{
    std::filesystem::path module;
    std::string           name;

    auto to_hash() const { return hash(module, name); }
    auto to_string() const -> std::string;
    auto operator==(const type_struct_template&) const -> bool = default;
};

// Only used during template argument type deduction
struct type_placeholder
{
    std::string name;

    auto to_hash() const { return hash(name); }
    auto to_string() const -> std::string;
    auto operator==(const type_placeholder&) const -> bool = default;
};

struct type_name : public std::variant<
    type_null,
    type_bool,
    type_char,
    type_i32,
    type_i64,
    type_u64,
    type_f64,
    type_type,
    type_arena,
    type_module,

    type_array,
    type_struct,
    type_ptr,
    type_span,
    type_optional,

    type_function_ptr,
    type_bound_method,
    type_bound_method_template,
    
    type_function,
    type_function_template,
    type_struct_template,
    type_placeholder>
{
    using variant::variant;
    type_name(const type_name&) = default;
    
    bool is_const = false;

    [[nodiscard]] auto add_ptr() const -> type_name;
    [[nodiscard]] auto remove_ptr() const -> type_name;

    [[nodiscard]] auto add_optional() const -> type_name;
    [[nodiscard]] auto remove_optional() const -> type_name;
 
    [[nodiscard]] auto add_const() const -> type_name;
    [[nodiscard]] auto remove_const() const -> type_name;

    [[nodiscard]] auto add_array(std::size_t size) const -> type_name;
    [[nodiscard]] auto remove_array() const -> type_name;

    [[nodiscard]] auto add_span() const -> type_name;
    [[nodiscard]] auto remove_span() const -> type_name;

    template <typename T> auto is()     const -> bool     { return std::holds_alternative<T>(*this); }
    template <typename T> auto as()     const -> const T& { return std::get<T>(*this); }
    template <typename T> auto get_if() const -> const T* { return std::get_if<T>(this); }

    auto to_hash() const -> std::size_t {
        return std::visit([](const auto& obj) { return hash(obj); }, *this);
    }

    auto to_string() const -> std::string {
        const auto inner = std::visit([](const auto& obj) { return obj.to_string(); }, *this);
        return std::format("{}{}", inner, is_const ? " const" : "");
    }
};

// Used for resolving template types. In the future could also be used for type aliases
using template_map = std::unordered_map<std::string, type_name>;

auto string_literal_type() -> type_name;

struct const_value : public std::variant<
    std::monostate,        // no value, null
    bool,                  // bool
    char,                  // char
    std::int32_t,          // i32
    std::int64_t,          // i64
    std::uint64_t,         // u64
    double,                // f64
    std::filesystem::path, // module
    type_name              // type
>
{
    using variant::variant;
    const_value(const const_value&) = default;

    template <typename T> auto is()     const -> bool     { return std::holds_alternative<T>(*this); }
    template <typename T> auto as()     const -> const T& { return std::get<T>(*this); }
    template <typename T> auto get_if() const -> const T* { return std::get_if<T>(this); }
    auto has_value()                    const -> bool     { return !is<std::monostate>(); }
};


}

template <>
struct std::formatter<std::byte> : std::formatter<std::string>
{
    auto format(std::byte b, auto& ctx) const {
        const auto str = std::format("{:X}", static_cast<unsigned char>(b));
        return std::formatter<std::string>::format(str, ctx);
    }
};