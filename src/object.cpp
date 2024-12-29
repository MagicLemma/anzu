#include "object.hpp"
#include "token.hpp"
#include "utility/common.hpp"

#include <cassert>
#include <algorithm>
#include <ranges>
#include <string_view>

namespace anzu {

auto type_function::to_pointer() const -> type_name
{
    return type_function_ptr{ param_types, return_type };
}

auto type_name::add_ptr() const -> type_name
{
    return { type_ptr{ .inner_type{*this} } };
}

auto type_name::remove_ptr() const -> type_name
{
    if (!is<type_ptr>()) panic("tried to remove_ptr on a non-ptr type\n");
    return *as<type_ptr>().inner_type;
}

auto type_name::add_optional() const -> type_name
{
    return { type_optional{ .inner_type{*this} } };
}

auto type_name::remove_optional() const -> type_name
{
    if (!is<type_optional>()) panic("tried to remove_optional on a non-optional type\n");
    return *as<type_optional>().inner_type;
}

auto type_name::add_const() const -> type_name
{
    auto copy = *this;
    copy.is_const = true;
    return copy;
}

auto type_name::remove_const() const -> type_name
{
    auto copy = *this;
    copy.is_const = false;
    return copy;
}

auto to_string_paren(const type_name& type) -> std::string
{
    const auto str = type.to_string();
    if (type.is<type_function_ptr>()) {
        return std::format("({})", str);
    }
    return str;
}

auto type_null::to_string() const -> std::string
{
    return "null";
}

auto type_bool::to_string() const -> std::string
{
    return "bool";
}

auto type_char::to_string() const -> std::string
{
    return "char";
}

auto type_i32::to_string() const -> std::string
{
    return "i32";
}

auto type_i64::to_string() const -> std::string
{
    return "i64";
}

auto type_u64::to_string() const -> std::string
{
    return "u64";
}

auto type_f64::to_string() const -> std::string
{
    return "f64";
}

auto type_type::to_string() const -> std::string
{
    return "type";
}

auto type_arena::to_string() const -> std::string
{
    return "arena";
}

auto type_module::to_string() const -> std::string
{
    return "module";
}

auto type_struct::to_string() const -> std::string
{
    if (!templates.empty()) {
        return std::format("<{}>.{}!({})", module.string(), name, format_comma_separated(templates));
    } else {
        return std::format("<{}>.{}", module.string(), name);
    }
}

auto type_array::to_string() const -> std::string
{
    return std::format("{}[{}]", to_string_paren(*inner_type), count);
}

auto type_ptr::to_string() const -> std::string
{
    return std::format("{}&", to_string_paren(*inner_type));
}

auto type_span::to_string() const -> std::string
{
    return std::format("{}[]", to_string_paren(*inner_type));
}

auto type_optional::to_string() const -> std::string
{
    return std::format("{}?", to_string_paren(*inner_type));
}

auto type_function_ptr::to_string() const -> std::string
{
    return std::format(
        "{}({}) -> {}",
        anzu::to_string(token_type::kw_function),
        format_comma_separated(param_types),
        to_string_paren(*return_type)
    );
}

auto type_bound_method::to_string() const -> std::string
{
    return std::format(
        "<bound_method: '{}({}) -> {}'>",
        anzu::to_string(token_type::kw_function),
        id,
        format_comma_separated(param_types),
        to_string_paren(*return_type)
    );
}

auto type_bound_method_template::to_string() const -> std::string
{
    return std::format(
        "<bound_method_template: <{}>.{}.{}>",
        module.string(),
        struct_name,
        name
    );
}

auto type_function::to_string() const -> std::string
{
    const auto function_ptr_type = type_function_ptr{param_types, return_type};
    return std::format("<function: id {} {}>", id, function_ptr_type);
}

auto type_function_template::to_string() const -> std::string
{
    return std::format("<function_template: <{}>.{}.{}>", module.string(), struct_name.name, name);
}

auto type_struct_template::to_string() const -> std::string
{
    return std::format("<struct_template: <{}>.{}>", module.string(), name);
}

auto type_placeholder::to_string() const -> std::string
{
    return std::format("<placeholder: {}>", name);
}

auto string_literal_type() -> type_name
{
    return type_name{type_char{}}.add_const().add_span();
}

auto type_name::add_array(std::size_t size) const -> type_name
{
    return {type_array{ .inner_type = { *this }, .count = size }};
}

auto type_name::remove_array() const -> type_name
{
    panic_if(!is<type_array>(), "Tried to strip array from non-array type {}", *this);
    return *as<type_array>().inner_type;
}

auto type_name::add_span() const -> type_name
{
    return {type_span{ .inner_type = { *this } }};
}

auto type_name::remove_span() const -> type_name
{
    panic_if(!is<type_span>(), "Tried to strip span from non-span type {}", *this);
    return *as<type_span>().inner_type;
}

}
