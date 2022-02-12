#include "ast.hpp"
#include "utility/print.hpp"

#include <functional>
#include <ranges>

namespace anzu {
namespace {

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

}

auto print_node(const anzu::node_expr& root, int indent) -> void
{
    const auto spaces = std::string(4 * indent, ' ');
    std::visit(overloaded {
        [&](const node_literal_expr& node) {
            anzu::print("{}Literal: {}\n", spaces, node.value.to_repr());
        },
        [&](const node_variable_expr& node) {
            anzu::print("{}Variable: {}\n", spaces, node.name);
        },
        [&](const node_bin_op_expr& node) {
            anzu::print("{}BinOp: \n", spaces);
            anzu::print("{}- Op: {}\n", spaces, node.token.text);
            anzu::print("{}- Lhs:\n", spaces);
            print_node(*node.lhs, indent + 1);
            anzu::print("{}- Rhs:\n", spaces);
            print_node(*node.rhs, indent + 1);
        },
        [&](const node_function_call_expr& node) {
            anzu::print("{}FunctionCall (Expr): {}\n", spaces, node.function_name);
            anzu::print("{}- Args:\n", spaces);
            for (const auto& arg : node.args) {
                print_node(*arg, indent + 1);
            }
        }
    }, root);
}

auto print_node(const anzu::node_stmt& root, int indent) -> void
{
    const auto spaces = std::string(4 * indent, ' ');
    std::visit(overloaded {
        [&](const node_sequence_stmt& node) {
            anzu::print("{}Sequence:\n", spaces);
            for (const auto& seq_node : node.sequence) {
                print_node(*seq_node, indent + 1);
            }
        },
        [&](const node_while_stmt& node) {
            anzu::print("{}While:\n", spaces);
            anzu::print("{}- Condition:\n", spaces);
            print_node(*node.condition, indent + 1);
            anzu::print("{}- Body:\n", spaces);
            print_node(*node.body, indent + 1);
        },
        [&](const node_if_stmt& node) {
            anzu::print("{}If:\n", spaces);
            anzu::print("{}- Condition:\n", spaces);
            print_node(*node.condition, indent + 1);
            anzu::print("{}- Body:\n", spaces);
            print_node(*node.body, indent + 1);
            if (node.else_body) {
                anzu::print("{}- Else:\n", spaces);
                print_node(*node.else_body, indent + 1);
            }
        },
        [&](const node_for_stmt& node) {
            anzu::print("{}For:\n", spaces);
            anzu::print("{}- Bind: {}\n",spaces, node.var);
            anzu::print("{}- Container:\n",spaces);
            print_node(*node.container, indent + 1);
            anzu::print("{}- Body:\n",spaces);
            print_node(*node.body, indent + 1);
        },
        [&](const node_break_stmt& node) {
            anzu::print("{}Break\n", spaces);
        },
        [&](const node_continue_stmt& node) {
            anzu::print("{}Continue\n", spaces);
        },
        [&](const node_assignment_stmt& node) {
            anzu::print("{}Assignment:\n", spaces);
            anzu::print("{}- Name: {}\n", spaces, node.name);
            anzu::print("{}- Value:\n", spaces);
            print_node(*node.expr, indent + 1);
        },
        [&](const node_function_def_stmt& node) {
            anzu::print("{}Function: {} (", spaces, node.name);
            anzu::print_comma_separated(node.sig.args, [](const auto& arg) {
                return std::format("{}: {}", arg.name, to_string(arg.type));
            });
            anzu::print(") -> {}\n", to_string(node.sig.return_type));
            print_node(*node.body, indent + 1);
        },
        [&](const node_function_call_stmt& node) {
            anzu::print("{}FunctionCall (Stmt): {}\n", spaces, node.function_name);
            anzu::print("{}- Args:\n", spaces);
            for (const auto& arg : node.args) {
                print_node(*arg, indent + 1);
            }
        },
        [&](const node_return_stmt& node) {
            anzu::print("{}Return:\n", spaces);
            print_node(*node.return_value, indent + 1);
        },
        [&](const node_debug_stmt& node) {
            anzu::print("{}--Debug--\n", spaces);
        }
    }, root);
}

}