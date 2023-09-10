#pragma once
#include "object.hpp"

#include <functional>
#include <string>
#include <vector>
#include <span>
#include <optional>

namespace anzu {

struct bytecode_context;
using builtin_function = std::function<void(bytecode_context&)>;

struct builtin
{
    std::string            name;
    builtin_function       ptr;
    std::vector<type_name> args;
    type_name              return_type;
};

auto get_builtins() -> std::span<const builtin>;
auto get_builtin(std::size_t id) -> const builtin&;

}