#include "functions.hpp"
#include "object.hpp"
#include "runtime.hpp"
#include "utility/print.hpp"
#include "utility/overloaded.hpp"

#include <unordered_map>
#include <string>
#include <functional>

namespace anzu {
namespace {

auto builtin_sqrt(std::span<const block> args) -> block
{
    const auto& val = std::get<block_float>(args[0]);
    return block_float{std::sqrt(val)};
}

template <typename T>
auto builtin_print(std::span<const block> args) -> block
{
    print("{}", std::get<T>(args[0]));
    return block{block_null{}};
}

template <typename T>
auto builtin_println(std::span<const block> args) -> block
{
    print("{}\n", std::get<T>(args[0]));
    return block{block_null{}};
}

auto builtin_print_char(std::span<const block> args) -> block
{
    print("{}", static_cast<char>(std::get<block_byte>(args[0])));
    return block{block_null{}};
}

auto builtin_println_char(std::span<const block> args) -> block
{
    print("{}\n", static_cast<char>(std::get<block_byte>(args[0])));
    return block{block_null{}};
}

auto builtin_print_null(std::span<const block> args) -> block
{
    print("null");
    return block{block_null{}};
}

auto builtin_println_null(std::span<const block> args) -> block
{
    print("null\n");
    return block{block_null{}};
}

auto builtin_put(std::span<const block> args) -> block
{
    anzu::print("{}", static_cast<char>(std::get<block_byte>(args[0])));
    return block{block_null{}};
}

}

auto construct_builtin_map() -> builtin_map
{
    auto builtins = builtin_map{};

    builtins.emplace(
        builtin_key{ .name = "sqrt", .args = { float_type() } },
        builtin_val{ .ptr = builtin_sqrt, .return_type = float_type() }
    );

    builtins.emplace(
        builtin_key{ .name = "print", .args = { int_type() } },
        builtin_val{ .ptr = builtin_print<block_int>, .return_type = null_type() }
    );
    builtins.emplace(
        builtin_key{ .name = "println", .args = { int_type() } },
        builtin_val{ .ptr = builtin_println<block_int>, .return_type = null_type() }
    );

    builtins.emplace(
        builtin_key{ .name = "print", .args = { uint_type() } },
        builtin_val{ .ptr = builtin_print<block_uint>, .return_type = null_type() }
    );
    builtins.emplace(
        builtin_key{ .name = "println", .args = { uint_type() } },
        builtin_val{ .ptr = builtin_println<block_uint>, .return_type = null_type() }
    );

    builtins.emplace(
        builtin_key{ .name = "print", .args = { char_type() } },
        builtin_val{ .ptr = builtin_print_char, .return_type = null_type() }
    );
    builtins.emplace(
        builtin_key{ .name = "println", .args = { char_type() } },
        builtin_val{ .ptr = builtin_println_char, .return_type = null_type() }
    );

    builtins.emplace(
        builtin_key{ .name = "print", .args = { float_type() } },
        builtin_val{ .ptr = builtin_print<block_float>, .return_type = null_type() }
    );
    builtins.emplace(
        builtin_key{ .name = "println", .args = { float_type() } },
        builtin_val{ .ptr = builtin_println<block_float>, .return_type = null_type() }
    );

    builtins.emplace(
        builtin_key{ .name = "print", .args = { bool_type() } },
        builtin_val{ .ptr = builtin_print<block_bool>, .return_type = null_type() }
    );
    builtins.emplace(
        builtin_key{ .name = "println", .args = { bool_type() } },
        builtin_val{ .ptr = builtin_println<block_bool>, .return_type = null_type() }
    );

    builtins.emplace(
        builtin_key{ .name = "print", .args = { null_type() } },
        builtin_val{ .ptr = builtin_print_null, .return_type = null_type() }
    );
    builtins.emplace(
        builtin_key{ .name = "println", .args = { null_type() } },
        builtin_val{ .ptr = builtin_println_null, .return_type = null_type() }
    );

    builtins.emplace(
        builtin_key{ .name = "put", .args = { char_type() } },
        builtin_val{ .ptr = builtin_put, .return_type = null_type() }
    );

    return builtins;
}

static const auto builtins = construct_builtin_map();

auto is_builtin(const std::string& name, const std::vector<type_name>& args) -> bool
{
    // Hack, generalise later
    if (name.starts_with("print") &&
        args.size() == 1 &&
        std::holds_alternative<type_list>(args[0]) &&
        inner_type(args[0]) == char_type()
    ) {
        return true;
    }
    return builtins.contains({name, args});
}

auto fetch_builtin(const std::string& name, const std::vector<type_name>& args) -> builtin_val
{
    // Hack, generalise later
    if (name.starts_with("print") &&
        args.size() == 1 &&
        std::holds_alternative<type_list>(args[0]) &&
        inner_type(args[0]) == char_type()
    ) {
        const auto newline = name == "println";
        const auto length = std::get<type_list>(args[0]).count;
        return builtin_val{
            .ptr = [=](std::span<const block> data) -> block {
                for (const auto& datum : data) {
                    print("{}", static_cast<char>(std::get<block_byte>(datum)));
                }
                if (newline) {
                    print("\n");
                }
                return block{block_null{}};
            },
            .return_type = null_type()
        };
    }

    auto it = builtins.find({name, args});
    if (it == builtins.end()) {
        anzu::print("builtin error: could not find function '{}({})'\n", name, format_comma_separated(args));
        std::exit(1);
    }
    return it->second;
}

}