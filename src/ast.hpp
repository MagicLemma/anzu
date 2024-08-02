#pragma once
#include "object.hpp"
#include "functions.hpp"
#include "token.hpp"

#include <variant>
#include <vector>
#include <memory>

namespace anzu {

struct node_expr;
using node_expr_ptr = std::shared_ptr<node_expr>;

struct node_stmt;
using node_stmt_ptr = std::shared_ptr<node_stmt>;

struct node_literal_i32_expr
{
    std::int32_t value;
    anzu::token  token;
};

struct node_literal_i64_expr
{
    std::int64_t value;
    anzu::token  token;
};

struct node_literal_u64_expr
{
    std::uint64_t value;
    anzu::token   token;
};

struct node_literal_f64_expr
{
    double      value;
    anzu::token token;
};

struct node_literal_char_expr
{
    char        value;
    anzu::token token;
};

struct node_literal_bool_expr
{
    bool        value;
    anzu::token token;
};

struct node_literal_null_expr
{
    anzu::token token;
};

struct node_literal_nullptr_expr
{
    anzu::token token;
};

struct node_literal_string_expr
{
    std::string value;
    anzu::token token;
};

struct node_name_expr
{
    std::string name;

    anzu::token token;
};

struct node_field_expr
{
    node_expr_ptr expr;
    std::string   field_name;

    anzu::token token;
};

struct node_unary_op_expr
{
    node_expr_ptr expr;

    anzu::token token;
};

struct node_binary_op_expr
{
    node_expr_ptr lhs;
    node_expr_ptr rhs;

    anzu::token token;
};

struct node_call_expr
{
    node_expr_ptr expr;
    std::vector<node_expr_ptr> template_args;
    std::vector<node_expr_ptr> args;

    anzu::token token;
};

struct node_member_call_expr
{
    node_expr_ptr              expr;
    std::string                function_name;
    std::vector<node_expr_ptr> template_args;
    std::vector<node_expr_ptr> other_args;

    anzu::token token;
};

struct node_array_expr
{
    std::vector<node_expr_ptr> elements;

    anzu::token token;
};

struct node_repeat_array_expr
{
    node_expr_ptr value;
    std::size_t   size;

    anzu::token token;
};

struct node_addrof_expr
{
    node_expr_ptr expr;

    anzu::token token;
};

struct node_deref_expr
{
    node_expr_ptr expr;
    
    anzu::token token;
};

struct node_sizeof_expr
{
    node_expr_ptr expr;

    anzu::token token;
};

struct node_subscript_expr
{
    node_expr_ptr expr;
    node_expr_ptr index;
    
    anzu::token token;
};

struct node_span_expr
{
    node_expr_ptr expr;
    node_expr_ptr lower_bound;
    node_expr_ptr upper_bound;
    
    anzu::token token;
};

struct node_typeof_expr
{
    node_expr_ptr expr;

    anzu::token token;
};

struct node_function_ptr_type_expr
{
    std::vector<node_expr_ptr> params;
    node_expr_ptr              return_type;

    anzu::token                token;
};

struct node_const_expr
{
    node_expr_ptr expr;

    anzu::token   token;
};

struct node_expr : std::variant<
    node_literal_i32_expr,
    node_literal_i64_expr,
    node_literal_u64_expr,
    node_literal_f64_expr,
    node_literal_char_expr,
    node_literal_bool_expr,
    node_literal_null_expr,
    node_literal_nullptr_expr,
    node_literal_string_expr,
    node_unary_op_expr,
    node_binary_op_expr,
    node_call_expr,
    node_member_call_expr,
    node_array_expr,
    node_repeat_array_expr,
    node_addrof_expr,
    node_sizeof_expr,
    node_span_expr,
    node_typeof_expr,
    node_function_ptr_type_expr,
    node_const_expr,
    node_name_expr,
    node_field_expr,
    node_deref_expr,
    node_subscript_expr>
{
};

struct node_parameter
{
    std::string   name;
    node_expr_ptr type;
};

struct node_signature
{
    std::vector<node_parameter> params;
    node_expr_ptr               return_type;
};

struct node_sequence_stmt
{
    std::vector<node_stmt_ptr> sequence;

    anzu::token token;
};

struct node_loop_stmt
{
    node_stmt_ptr body;

    anzu::token token;
};

struct node_while_stmt
{
    node_expr_ptr condition;
    node_stmt_ptr body;

    anzu::token token;
};

struct node_for_stmt
{
    std::string name;
    node_expr_ptr iter;
    node_stmt_ptr body;

    anzu::token token;
};

struct node_if_stmt
{
    node_expr_ptr condition;
    node_stmt_ptr body;
    node_stmt_ptr else_body;

    anzu::token token;
};

struct node_struct_stmt
{
    std::string                 name;
    std::vector<node_parameter> fields;
    std::vector<node_stmt_ptr>  functions;

    anzu::token token;
};

struct node_break_stmt
{
    anzu::token token;
};

struct node_continue_stmt
{
    anzu::token token;
};

struct node_declaration_stmt
{
    std::string   name;
    node_expr_ptr expr;
    node_expr_ptr explicit_type;
    bool          add_const;

    anzu::token token;
};

struct node_arena_declaration_stmt
{
    std::string name;
    anzu::token token;
};

struct node_assignment_stmt
{
    node_expr_ptr position;
    node_expr_ptr expr;

    anzu::token token;
};

struct node_function_def_stmt
{
    std::string              name;
    std::vector<std::string> template_types;
    node_signature           sig;
    node_stmt_ptr            body;

    anzu::token token;
};

struct node_member_function_def_stmt
{
    std::string              struct_name;
    std::string              function_name;
    std::vector<std::string> template_types;
    node_signature           sig;
    node_stmt_ptr            body;

    anzu::token token;
};

struct node_expression_stmt
{
    node_expr_ptr expr;

    anzu::token token;
};

struct node_return_stmt
{
    node_expr_ptr return_value;

    anzu::token token;
};

struct node_delete_stmt
{
    node_expr_ptr expr;

    anzu::token token;
};

struct node_assert_stmt
{
    node_expr_ptr expr;

    anzu::token token;
};

struct node_print_stmt
{
    std::string message;
    std::vector<node_expr_ptr> args;

    anzu::token token;
};


struct node_stmt : std::variant<
    node_sequence_stmt,
    node_loop_stmt,
    node_while_stmt,
    node_for_stmt,
    node_if_stmt,
    node_struct_stmt,
    node_break_stmt,
    node_continue_stmt,
    node_declaration_stmt,
    node_arena_declaration_stmt,
    node_assignment_stmt,
    node_member_function_def_stmt,
    node_function_def_stmt,
    node_expression_stmt,
    node_return_stmt,
    node_assert_stmt,
    node_print_stmt>
{
};

auto is_lvalue_expr(const node_expr& expr) -> bool;
auto is_rvalue_expr(const node_expr& expr) -> bool;

auto print_node(const anzu::node_expr& root, int indent = 0) -> void;
auto print_node(const anzu::node_stmt& root, int indent = 0) -> void;

}