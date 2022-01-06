#include "op_codes.hpp"

#include <iostream>
#include <string>

namespace anzu {
namespace {

// Move these elsewhere
bool is_literal(const std::string& token)
{
    return token == "false"
        || token == "true"
        || token.find_first_not_of("0123456789") == std::string::npos;
}

anzu::stack_frame::type parse_literal(const std::string& token)
{
    if (token == "true") {
        return true;
    }
    if (token == "false") {
        return false;
    }
    if (token.find_first_not_of("0123456789") == std::string::npos) {
        return std::stoi(token);
    }
    fmt::print("[Fatal] Could not parse constant: {}\n", token);
    std::exit(1);
}

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

}

int op_store::apply(anzu::stack_frame& frame) const
{
    frame.load(name, frame.pop());
    return 1;
}

int op_dump::apply(anzu::stack_frame& frame) const
{
    anzu::print_value(frame.pop());
    fmt::print("\n");
    return 1;
}

int op_pop::apply(anzu::stack_frame& frame) const
{
    frame.pop();
    return 1;
}

int op_push_const::apply(anzu::stack_frame& frame) const
{
    frame.push(value);
    return 1;
}

int op_push_var::apply(anzu::stack_frame& frame) const
{
    frame.push(frame.fetch(name));
    return 1;
}

int op_add::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, int> && std::is_same_v<B, int>) {
            frame.push(a + b);
        } else {
            fmt::print("Can only add integers\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_sub::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, int> && std::is_same_v<B, int>) {
            frame.push(a - b);
        } else {
            fmt::print("Can only sub integers\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_mul::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, int> && std::is_same_v<B, int>) {
            frame.push(a * b);
        } else {
            fmt::print("Can only add integers\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_div::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, int> && std::is_same_v<B, int>) {
            frame.push(a / b);
        } else {
            fmt::print("Can only sub integers\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_mod::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, int> && std::is_same_v<B, int>) {
            frame.push(a % b);
        } else {
            fmt::print("Can only sub integers\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_dup::apply(anzu::stack_frame& frame) const
{
    frame.push(frame.peek());
    return 1;
}

int op_print_frame::apply(anzu::stack_frame& frame) const
{
    frame.print();
    return 1;
}

int op_if::apply(anzu::stack_frame& frame) const
{
    return 1;
}

int op_while::apply(anzu::stack_frame& frame) const
{
    return 1;
}

int op_do::apply(anzu::stack_frame& frame) const
{
    auto condition = std::visit(overloaded {
        [](int v) { return v != 0; },
        [](bool v) { return v; }
    }, frame.pop());
    return condition ? 1 : jump;
}

int op_else::apply(anzu::stack_frame& frame) const
{
    return jump;
}

int op_end::apply(anzu::stack_frame& frame) const
{
    return jump;
}

int op_eq::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, B>) {
            frame.push(a == b);
        } else {
            fmt::print("Can only compare values of the same type\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_ne::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, B>) {
            frame.push(a != b);
        } else {
            fmt::print("Can only compare values of the same type\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_lt::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, B>) {
            frame.push(a < b);
        } else {
            fmt::print("Can only compare values of the same type\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_le::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, B>) {
            frame.push(a <= b);
        } else {
            fmt::print("Can only compare values of the same type\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_gt::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, B>) {
            frame.push(a > b);
        } else {
            fmt::print("Can only compare values of the same type\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_ge::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, B>) {
            frame.push(a >= b);
        } else {
            fmt::print("Can only compare values of the same type\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_or::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, bool> && std::is_same_v<B, bool>) {
            frame.push(a || b);
        } else {
            fmt::print("Logical OR can only be used on bools\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_and::apply(anzu::stack_frame& frame) const
{
    auto b = frame.pop();
    auto a = frame.pop();
    std::visit([&]<typename A, typename B>(const A& a, const B& b) {
        if constexpr (std::is_same_v<A, bool> && std::is_same_v<B, bool>) {
            frame.push(a && b);
        } else {
            fmt::print("Logical AND can only be used on bools\n");
            std::exit(1);
        }
    }, a, b);
    return 1;
}

int op_input::apply(anzu::stack_frame& frame) const
{
    frame.pop();
    fmt::print("Input: ");
    std::string in;
    std::cin >> in;
    if (!is_literal(in)) {
        fmt::print("[BAD INPUT]\n");
        std::exit(1);
    }
    frame.push(parse_literal(in));
    return 1;
}

}