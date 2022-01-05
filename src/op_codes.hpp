#pragma once
#include "stack_frame.hpp"

#include <fmt/format.h>
#include <variant>

namespace anzu {
namespace op {

struct dump
{
    void print() const { fmt::print("OP_DUMP\n"); }
    int apply(anzu::stack_frame& frame) const;
};

struct pop
{
    void print() const { fmt::print("OP_POP\n"); }
    int apply(anzu::stack_frame& frame) const;
};

struct push_const
{
    anzu::stack_frame::type value;

    void print() const {
        fmt::print("OP_PUSH_CONST(");
        anzu::print_value(value);
        fmt::print(")\n"); }
    int apply(anzu::stack_frame& frame) const;
};

struct store_const
{
    std::string name;
    anzu::stack_frame::type value;

    void print() const {
        fmt::print("OP_STORE_CONST({}, ", name);
        anzu::print_value(value);
        fmt::print(")\n"); }
    int apply(anzu::stack_frame& frame) const;
};

struct push_var
{
    std::string name;

    void print() const { fmt::print("OP_PUSH_VAR({})\n", name); }
    int apply(anzu::stack_frame& frame) const;
};

struct store_var
{
    std::string name;
    std::string source; 

    void print() const { fmt::print("OP_STORE_VAR({}, {})\n", name, source); }
    int apply(anzu::stack_frame& frame) const;
};

struct add
{
    void print() const { fmt::print("OP_ADD\n"); }
    int apply(anzu::stack_frame& frame) const;
};

struct sub
{
    void print() const { fmt::print("OP_SUB\n"); }
    int apply(anzu::stack_frame& frame) const;
};

struct dup
{
    void print() const { fmt::print("OP_DUP\n"); }
    int apply(anzu::stack_frame& frame) const;
};

struct print_frame
{
    void print() const { fmt::print("OP_PRINT_FRAME\n"); }
    int apply(anzu::stack_frame& frame) const;
};

struct begin_if
{
    int jump;

    void print() const { fmt::print("OP_BEGIN_IF({})\n", jump); }
    int apply(anzu::stack_frame& frame) const;
};

struct else_if
{
    int jump;

    void print() const { fmt::print("OP_ELSE_IF({})\n", jump); }
    int apply(anzu::stack_frame& frame) const;
};

struct end_if
{
    void print() const { fmt::print("OP_END_IF\n"); }
    int apply(anzu::stack_frame& frame) const;
};

struct equals
{
    void print() const { fmt::print("OP_EQUALS\n"); }
    int apply(anzu::stack_frame& frame) const;
};

}

using opcode = std::variant<
    op::dump,
    op::pop,
    op::push_const,
    op::store_const,
    op::push_var,
    op::store_var,
    op::add,
    op::sub,
    op::dup,
    op::print_frame,
    op::begin_if,
    op::else_if,
    op::end_if,
    op::equals
>;

}