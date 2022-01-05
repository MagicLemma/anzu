#include "op_codes.hpp"

namespace anzu {

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

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

int op_store_const::apply(anzu::stack_frame& frame) const
{
    frame.load(name, value);
    return 1;
}

int op_push_var::apply(anzu::stack_frame& frame) const
{
    frame.push(frame.fetch(name));
    return 1;
}

int op_store_var::apply(anzu::stack_frame& frame) const
{
    frame.load(name, frame.fetch(source));
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

int op_equals::apply(anzu::stack_frame& frame) const
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

int op_not_equals::apply(anzu::stack_frame& frame) const
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

}