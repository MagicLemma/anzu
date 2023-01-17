#include "ast.hpp"
#include "utility/print.hpp"
#include "utility/overloaded.hpp"

#include <functional>
#include <ranges>

namespace anzu {

auto print_node(const node_expr& root, int indent) -> void
{
    const auto spaces = std::string(4 * indent, ' ');
    std::visit(overloaded {
        [&](const node_literal_expr& node) {
            print("{}Literal: {}\n", spaces, node.value);
        },
        [&](const node_variable_expr& node) {
            print("{}Variable: {}\n", spaces, node.name);
        },
        [&](const node_field_expr& node) {
            print("{}Field: \n", spaces);
            print("{}- Expr:\n", spaces);
            print_node(*node.expr, indent + 1);
            print("{}- Field: {}\n", spaces, node.field_name);
        },
        [&](const node_unary_op_expr& node) {
            print("{}UnaryOp: \n", spaces);
            print("{}- Op: {}\n", spaces, node.token.text);
            print("{}- Expr:\n", spaces);
            print_node(*node.expr, indent + 1);
        },
        [&](const node_binary_op_expr& node) {
            print("{}BinaryOp: \n", spaces);
            print("{}- Op: {}\n", spaces, node.token.text);
            print("{}- Lhs:\n", spaces);
            print_node(*node.lhs, indent + 1);
            print("{}- Rhs:\n", spaces);
            print_node(*node.rhs, indent + 1);
        },
        [&](const node_function_call_expr& node) {
            print("{}FunctionCall: {}\n", spaces, node.function_name);
            print("{}- Args:\n", spaces);
            for (const auto& arg : node.args) {
                print_node(*arg, indent + 1);
            }
        },
        [&](const node_member_function_call_expr& node) {
            print("{}MemberFunctionCall: {}\n", spaces, node.function_name);
            print("{}- Object:\n", spaces);
            print_node(*node.expr, indent + 1);
            print("{}- Args:\n", spaces);
            for (const auto& arg : node.args) {
                print_node(*arg, indent + 1);
            }
        },
        [&](const node_list_expr& node) {
            print("{}List:\n", spaces);
            print("{}- Elements:\n", spaces);
            for (const auto& element : node.elements) {
                print_node(*element, indent + 1);
            }
        },
        [&](const node_repeat_list_expr& node) {
            print("{}List:\n", spaces);
            print("{}- Element:\n", spaces);
            print_node(*node.value, indent + 1);
            print("{}- Count: {}\n", spaces, node.size);
        },
        [&](const node_addrof_expr& node) {
            print("{}AddrOf:\n", spaces);
            print_node(*node.expr, indent + 1);
        },
        [&](const node_deref_expr& node) {
            print("{}Deref:\n", spaces);
            print_node(*node.expr, indent + 1);
        },
        [&](const node_sizeof_expr& node) {
            print("{}SizeOf:\n", spaces);
            print_node(*node.expr, indent + 1);
        },
        [&](const node_subscript_expr& node) {
            print("{}Subscript:\n", spaces);
            print("{}- Expr:\n", spaces);
            print_node(*node.expr, indent + 1);
            print("{}- Index:\n", spaces);
            print_node(*node.index, indent + 1);
        },
        [&](const node_new_expr& node) {
            print("{}New {}:\n", spaces, node.type);
            print("{}- Size:\n", spaces);
            print_node(*node.size, indent + 1);
        }
    }, root);
}

auto print_node(const node_stmt& root, int indent) -> void
{
    const auto spaces = std::string(4 * indent, ' ');
    std::visit(overloaded {
        [&](const node_sequence_stmt& node) {
            print("{}Sequence:\n", spaces);
            for (const auto& seq_node : node.sequence) {
                print_node(*seq_node, indent + 1);
            }
        },
        [&](const node_loop_stmt& node) {
            print("{}Loop:\n", spaces);
            print("{}- Body:\n", spaces);
            print_node(*node.body, indent + 1);
        },
        [&](const node_while_stmt& node) {
            print("{}While:\n", spaces);
            print("{}- Condition:\n", spaces);
            print_node(*node.condition, indent + 1);
            print("{}- Body:\n", spaces);
            print_node(*node.body, indent + 1);
        },
        [&](const node_for_stmt& node) {
            print("{}For (name={}):\n", spaces, node.name);
            print("{}- Iter:\n", spaces);
            print_node(*node.iter, indent + 1);
            print("{}- Body:\n", spaces);
            print_node(*node.body, indent + 1);
        },
        [&](const node_if_stmt& node) {
            print("{}If:\n", spaces);
            print("{}- Condition:\n", spaces);
            print_node(*node.condition, indent + 1);
            print("{}- Body:\n", spaces);
            print_node(*node.body, indent + 1);
            if (node.else_body) {
                print("{}- Else:\n", spaces);
                print_node(*node.else_body, indent + 1);
            }
        },
        [&](const node_struct_stmt& node) {
            print("{}Struct:\n", spaces);
            print("{}- Name: {}\n", spaces, node.name);
            print("{}- Fields:\n", spaces);
            for (const auto& field : node.fields) {
                print("{}  - {}: {}\n", spaces, field.name, field.type);
            }
            print("{}- MemberFunctions:\n", spaces);
            for (const auto& function : node.functions) {
                print_node(*function, indent + 1);
            }
        },
        [&](const node_break_stmt& node) {
            print("{}Break\n", spaces);
        },
        [&](const node_continue_stmt& node) {
            print("{}Continue\n", spaces);
        },
        [&](const node_declaration_stmt& node) {
            print("{}Declaration:\n", spaces);
            print("{}- Name: {}\n", spaces, node.name);
            print("{}- Value:\n", spaces);
            print_node(*node.expr, indent + 1);
        },
        [&](const node_assignment_stmt& node) {
            print("{}Assignment:\n", spaces);
            print("{}- Name:\n", spaces);
            print_node(*node.position, indent + 1);
            print("{}- Value:\n", spaces);
            print_node(*node.expr, indent + 1);
        },
        [&](const node_function_def_stmt& node) {
            print("{}Function: {} (", spaces, node.name);
            print_comma_separated(node.sig.params, [](const auto& arg) {
                return std::format("{}: {}", arg.name, arg.type);
            });
            print(") -> {}\n", node.sig.return_type);
            print_node(*node.body, indent + 1);
        },
        [&](const node_member_function_def_stmt& node) {
            print("{}MemberFunction: {}::{} (", spaces, node.struct_name, node.function_name);
            print_comma_separated(node.sig.params, [](const auto& arg) {
                return std::format("{}: {}", arg.name, arg.type);
            });
            print(") -> {}\n", node.sig.return_type);
            print_node(*node.body, indent + 1);
        },
        [&](const node_expression_stmt& node) {
            print("{}Expression:\n", spaces);
            print_node(*node.expr, indent + 1);
        },
        [&](const node_return_stmt& node) {
            print("{}Return:\n", spaces);
            print_node(*node.return_value, indent + 1);
        },
        [&](const node_delete_stmt& node) {
            print("{}Delete:\n", spaces);
            print_node(*node.expr, indent + 1);
        }
    }, root);
}

auto is_lvalue_expr(const node_expr& expr) -> bool
{
    return std::holds_alternative<node_variable_expr>(expr)
        || std::holds_alternative<node_field_expr>(expr)
        || std::holds_alternative<node_deref_expr>(expr)
        || std::holds_alternative<node_subscript_expr>(expr);
}

auto is_rvalue_expr(const node_expr& expr) -> bool
{
    return !is_lvalue_expr(expr);
}

}