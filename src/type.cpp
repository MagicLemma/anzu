#include "type.hpp"
#include "utility/print.hpp"
#include "utility/overloaded.hpp"

#include <algorithm>
#include <cassert>

namespace anzu {

auto to_string(const type& type) -> std::string
{
    return std::visit([](const auto& t) { return to_string(t); }, type);
}

auto to_string(const type_simple& type) -> std::string
{
    return type.name;
}

auto to_string(const type_compound& type) -> std::string
{
    const auto subtypes = format_comma_separated(
        type.subtypes,
        [](const auto& t) { return to_string(t); }
    );
    return std::format("{}<{}>", type.name, subtypes);
}

auto to_string(const type_generic& type) -> std::string
{
    return std::format("[{}]", type.id);
}

auto hash(const type& type) -> std::size_t
{
    return std::visit([](const auto& t) { return hash(t); }, type);
}

auto hash(const type_simple& type) -> std::size_t
{
    return std::hash<std::string>{}(type.name);
}

auto hash(const type_compound& type) -> std::size_t
{
    auto hash_value = std::hash<std::string>{}(type.name);
    for (const auto& subtype : type.subtypes) {
        hash_value ^= hash(subtype);
    }
    return hash_value;
}

auto hash(const type_generic& type) -> std::size_t
{
    return std::hash<int>{}(type.id);
}

auto int_type()  -> type
{
    return {type_simple{ .name = std::string{tk_int}  }};
}

auto bool_type() -> type
{
    return {type_simple{ .name = std::string{tk_bool} }};
}

auto str_type()  -> type
{
    return {type_simple{ .name = std::string{tk_str}  }};
}

auto null_type() -> type
{
    return {type_simple{ .name = std::string{tk_null} }};
}

auto generic_type(int id) -> type
{
    return {type_generic{ .id = id }};
}

auto concrete_list_type(const type& t) -> type
{
    return {type_compound{
        .name = std::string{tk_list}, .subtypes = { t }
    }};
}

auto generic_list_type() -> type
{
    return {type_compound{
        .name = std::string{tk_list}, .subtypes = { generic_type(0) }
    }};
}

auto is_type_complete(const type& t) -> bool
{
    return std::visit(overloaded {
        [](const type_simple&) {
            return true;
        },
        [](const type_generic&) {
            return false;
        },
        [](const type_compound& t) {
            return std::all_of(begin(t.subtypes), end(t.subtypes), [](const auto& st) {
                return is_type_complete(st);
            });
        }
    }, t);
}

// Loads each key/value pair from src into dst. If the key already exists in dst and has a
// different value, stop and return false.
auto update(
    std::unordered_map<int, type>& dst, const std::unordered_map<int, type>& src
)
    -> bool
{
    for (const auto& [key, value] : src) {
        if (auto it = dst.find(key); it != dst.end()) {
            if (it->second != value) {
                return false;
            }
        } else {
            dst.emplace(key, value);
        }
    }
    return true;
}

auto match(const type& concrete, const type& pattern) -> std::optional<match_result>
{
    // Pre-condition, concrete must be a complete type (non-generic and no generic subtypes)
    if (!is_type_complete(concrete)) {
        anzu::print("cannot match the incomplete type '{}'\n", to_string(concrete));
        std::exit(1);
    }

    // Check 1: Trivial case - pattern is generic, matches entire concrete type
    if (const auto inner = std::get_if<type_generic>(&pattern)) {
        return match_result{
            { inner->id, concrete }
        };
    }

    // At this point, neither 'concrete' nor 'pattern' are generic

    // Check 2: Trivial case - equality implies match
    if (concrete == pattern) {
        return match_result{};
    }

    // If either are simple, there there is no match because:
    //   - if both are simple, they are not equal (check 2)
    //   - simple cannot match compound and vice versa
    if (std::holds_alternative<type_simple>(pattern) || std::holds_alternative<type_simple>(concrete)) {
        return std::nullopt; // No match
    }

    // At this point, both 'concrete' and 'pattern' are compound
    const auto& p = std::get<type_compound>(pattern);
    const auto& c = std::get<type_compound>(concrete);
    if (p.name != c.name || p.subtypes.size() != c.subtypes.size()) {
        return std::nullopt;
    }

    auto matches = match_result{};

    // Loop through the subtypes and do pairwise matches. Any successful matches should be
    // lifted into our match map. If an index is already in our map with a different type,
    // the match fails and we return nullopt. (std::views::zip in C++23 would be nice here)
    auto cit = c.subtypes.begin();
    auto pit = p.subtypes.begin();
    for (; cit != c.subtypes.end(); ++cit, ++pit) {
        const auto submatch = match(*cit, *pit);
        if (!submatch.has_value()) {
            return std::nullopt;
        }
        if (!update(matches, submatch.value())) {
            return std::nullopt;
        }
    }

    return matches;
}

auto replace(type& ret, const match_result& matches) -> void
{
    std::visit(overloaded {
        [&](type_simple&) {},
        [&](type_generic& type) {
            if (auto it = matches.find(type.id); it != matches.end()) {
                ret = it->second;
            }
        },
        [&](type_compound& type) {
            for (auto& subtype : type.subtypes) {
                replace(subtype, matches);
            }
        }
    }, ret);
}

auto bind_generics(const type& incomplete, const std::unordered_map<int, type>& matches) -> type
{
    auto ret_type = incomplete;
    replace(ret_type, matches);
    return ret_type;
}

auto to_string(const signature& sig) -> std::string
{
    return std::format(
        "({}) -> {}",
        format_comma_separated(
            sig.args,
            [](const auto& arg) { return to_string(arg.type); }
        ),
        to_string(sig.return_type)
    );
}
type_store::type_store()
{
    d_types.emplace(int_type());
    d_types.emplace(bool_type());
    d_types.emplace(str_type());
    d_types.emplace(null_type());

    d_generics.emplace(generic_list_type());
}

auto type_store::is_registered_type(const type& t) const -> bool
{
    if (d_types.contains(t)) {
        return true;
    }

    if (d_generics.contains(t)) {
        return true;
    }

    return std::any_of(begin(d_generics), end(d_generics), [&](const auto& generic) {
        return match(t, generic).has_value();
    });
}

}