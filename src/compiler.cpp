#include "compiler.hpp"

#include "lexer.hpp"
#include "object.hpp"
#include "parser.hpp"
#include "functions.hpp"
#include "utility/common.hpp"
#include "utility/memory.hpp"

#include <string_view>
#include <optional>
#include <tuple>
#include <vector>
#include <stack>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace anzu {
namespace {

static const auto global_namespace = make_type("<global>");

auto push_expr_ptr(compiler& com, const node_expr& node) -> type_name;
auto push_expr_val(compiler& com, const node_expr& expr) -> type_name;
auto push_stmt(compiler& com, const node_stmt& root) -> void;
auto type_of_expr(compiler& com, const node_expr& node) -> type_name;

auto new_function(compiler& com, const std::string& name, const token& tok)
{
    if (com.in_function) tok.error("cannot create a new function while one is already being compiled");
    const auto id = com.compiled_functions.size();

    // The function signature can only be filled in after declaring the function parameters
    // since the types of some may depend on earlier parameters via typeof
    com.compiled_functions.emplace_back(name, signature{}, tok, id);

    if (com.functions_by_name.contains(name)) tok.error("a function with the name '{}' already exists", name);
    com.functions_by_name.emplace(name, id);
    com.in_function = true;
}

auto finish_function(compiler& com)
{
    com.in_function = false;
}


auto resolve_type(compiler& com, const token& tok, const node_type_ptr& type) -> type_name
{
    if (!type) {
        return global_namespace;
    }

    const auto resolved_type = std::visit(overloaded {
        [&](const node_named_type& node) {
            return node.type;
        },
        [&](const node_expr_type& node) {
            return type_of_expr(com, *node.expr);
        }
    }, *type);

    tok.assert(com.types.contains(resolved_type), "{} is not a recognised type", resolved_type);
    return resolved_type;
}

auto get_function(
    const compiler& com, const std::string& struct_name, const std::string& function_name
)
    -> std::optional<function_info>
{
    const auto full_name = std::format("{}::{}", struct_name, function_name);
    if (const auto it = com.functions_by_name.find(full_name); it != com.functions_by_name.end()) {
        return com.compiled_functions[it->second];
    }
    return std::nullopt;
}

auto push_function_call(compiler& com, const function_info& function) -> void
{
    auto args_size = std::size_t{0};
    for (const auto& param : function.sig.params) {
        args_size += com.types.size_of(param);
    }
    push_value(com.code(), op::push_u64, function.id, op::call, args_size);
}

// Registers the given name in the current scope
void declare_var(compiler& com, const token& tok, const std::string& name, const type_name& type)
{
    if (!com.variables.declare(name, type, com.types.size_of(type))) {
        tok.error("name already in use: '{}'", name);
    }
}

auto push_var_addr(compiler& com, const token& tok, const std::string& name) -> type_name
{
    const auto var = com.variables.find(name);
    tok.assert(var.has_value(), "could not find variable '{}'\n", name);
    const auto op = var->is_local ? op::push_ptr_local : op::push_ptr_global;
    push_value(com.code(), op, var->location);
    return var->type;
}

auto load_variable(compiler& com, const token& tok, const std::string& name) -> void
{
    const auto type = push_var_addr(com, tok, name);
    push_value(com.code(), op::load, com.types.size_of(type));
}

auto save_variable(compiler& com, const token& tok, const std::string& name) -> void
{
    const auto type = push_var_addr(com, tok, name);
    push_value(com.code(), op::save, com.types.size_of(type));
}

// Given a type and a field name, push the offset of the fields position relative to its
// owner onto the stack
auto push_field_offset(
    compiler& com, const token& tok, const type_name& type, const std::string& field_name
)
    -> type_name
{
    auto offset = std::size_t{0};
    for (const auto& field : com.types.fields_of(type)) {
        if (field.name == field_name) {
            push_value(com.code(), op::push_u64, offset);
            return field.type;
        }
        offset += com.types.size_of(field.type);
    }
    
    tok.error("could not find field '{}' for type '{}'\n", field_name, type);
}

void verify_sig(
    const token& tok,
    const std::vector<type_name>& expected,
    const std::vector<type_name>& actual)
{
    if (expected.size() != actual.size()) {
        tok.error("function expected {} args, got {}", expected.size(), actual.size());
    }

    auto arg_index = std::size_t{0};
    for (const auto& [expected_param, actual_param] : zip(expected, actual)) {
        if (actual_param != expected_param) {
            tok.error("arg {} type '{}' does not match '{}'", arg_index, actual_param, expected_param);
            ++arg_index;
        }
    }
}

auto get_constructor_params(const compiler& com, const type_name& type) -> std::vector<type_name>
{
    if (type.is_fundamental()) {
        return {type};
    }
    auto params = std::vector<type_name>{};
    for (const auto& field : com.types.fields_of(type)) {
        params.emplace_back(field.type);
    }
    return params;
}

// Gets the type of the expression by compiling it, then removes the added
// op codes to leave the program unchanged before returning the type.
auto type_of_expr(compiler& com, const node_expr& node) -> type_name
{
    const auto program_size = com.code().size();
    const auto type = push_expr_val(com, node);
    com.code().resize(program_size);
    return type;
}

// Fetches the given literal from read only memory, or adds it if it is not there, and
// returns the pointer.
auto insert_into_rom(compiler& com, std::string_view data) -> std::size_t
{
    const auto index = com.rom.find(data);
    if (index != std::string::npos) {
        return index;
    }
    const auto ptr = com.rom.size();
    com.rom.append(data);
    return ptr;
}

auto push_assert(compiler& com, std::string_view message) -> void
{
    const auto index = insert_into_rom(com, message);
    push_value(com.code(), op::assert, index, message.size());
}

auto push_expr_ptr(compiler& com, const node_name_expr& node) -> type_name
{
    if (auto func = get_function(com, to_string(global_namespace), node.name)) {
        node.token.error("cannot take address of a function pointer");
    }

    return push_var_addr(com, node.token, node.name);
}

// I think this is a bit of a hack; when pushing the value of a function pointer, we need
// to do it in a special way.
auto push_expr_val(compiler& com, const node_name_expr& node) -> type_name
{
    if (auto func = get_function(com, to_string(global_namespace), node.name)) {
        const auto& info = *func;
        push_value(com.code(), op::push_u64, info.id);

        // next, construct the return type.
        const auto ptr_type = type_function_ptr{
            .param_types = info.sig.params,
            .return_type = make_value<type_name>(info.sig.return_type)
        };
        return ptr_type;
    }

    // This is the default logic for pushing an lvalue.
    const auto type = push_expr_ptr(com, node);
    push_value(com.code(), op::load, com.types.size_of(type));
    return type;
}

auto push_expr_ptr(compiler& com, const node_field_expr& node) -> type_name
{
    auto type = push_expr_ptr(com, *node.expr).remove_const();

    // Allow for field access on a pointer. ALso strip away constness at each
    // step since wrapping a type in const will stop this from stripping away
    // further pointers.
    while (type.is_ptr()) {
        while (type.is_const()) type = type.remove_const();
        push_value(com.code(), op::load, sizeof(std::byte*));
        type = type.remove_ptr();
    }

    const auto field_type = push_field_offset(com, node.token, type, node.field_name);
    push_value(com.code(), op::u64_add); // modify ptr
    if (type.is_const()) return field_type.add_const(); // propagate const to fields
    return field_type;
}

auto push_expr_ptr(compiler& com, const node_deref_expr& node) -> type_name
{
    const auto [type, is_const] = push_expr_val(com, *node.expr).strip_const(); // Push the address
    node.token.assert(type.is_ptr(), "cannot use deref operator on non-ptr type '{}'", type);
    return type.remove_ptr();
}

auto push_expr_ptr(compiler& com, const node_subscript_expr& node) -> type_name
{
    const auto expr_type = type_of_expr(com, *node.expr);
    const auto [real_type, is_const] = expr_type.strip_const();

    const auto is_array = real_type.is_array();
    const auto is_span = real_type.is_span();
    node.token.assert(is_array || is_span, "subscript only supported for arrays and spans");

    push_expr_ptr(com, *node.expr);

    // If we are a span, we want the address that it holds rather than its own address,
    // so switch the pointer by loading what it's pointing at.
    if (is_span) {
        push_value(com.code(), op::load, sizeof(std::byte*));
    }

    // Offset pointer by (index * size)
    const auto inner = inner_type(real_type);
    const auto index = push_expr_val(com, *node.index);
    node.token.assert_eq(index, u64_type(), "subscript argument must be u64, got {}", index);
    push_value(com.code(), op::push_u64, com.types.size_of(inner));
    push_value(com.code(), op::u64_mul);
    push_value(com.code(), op::u64_add); // modify ptr
    if (is_array && is_const) {
        return inner.add_const();
    }
    return inner;
}

[[noreturn]] auto push_expr_ptr(compiler& com, const auto& node) -> type_name
{
    node.token.error("cannot take address of a non-lvalue\n");
}

auto push_expr_ptr(compiler& com, const node_expr& node) -> type_name
{
    return std::visit([&](const auto& expr) { return push_expr_ptr(com, expr); }, node);
}

auto push_expr_val(compiler& com, const node_literal_i32_expr& node) -> type_name
{
    push_value(com.code(), op::push_i32, node.value);
    return i32_type();
}

auto push_expr_val(compiler& com, const node_literal_i64_expr& node) -> type_name
{
    push_value(com.code(), op::push_i64, node.value);
    return i64_type();
}

auto push_expr_val(compiler& com, const node_literal_u64_expr& node) -> type_name
{
    push_value(com.code(), op::push_u64, node.value);
    return u64_type();
}

auto push_expr_val(compiler& com, const node_literal_f64_expr& node) -> type_name
{
    push_value(com.code(), op::push_f64, node.value);
    return f64_type();
}

auto push_expr_val(compiler& com, const node_literal_char_expr& node) -> type_name
{
    push_value(com.code(), op::push_char, node.value);
    return char_type();
}

auto push_expr_val(compiler& com, const node_literal_string_expr& node) -> type_name
{
    push_value(com.code(), op::push_string_literal);
    push_value(com.code(), insert_into_rom(com, node.value), node.value.size());
    return char_type().add_const().add_span();
}

auto push_expr_val(compiler& com, const node_literal_bool_expr& node) -> type_name
{
    push_value(com.code(), op::push_bool, node.value);
    return bool_type();
}

auto push_expr_val(compiler& com, const node_literal_null_expr& node) -> type_name
{
    push_value(com.code(), op::push_null);
    return null_type();
}

auto push_expr_val(compiler& com, const node_literal_nullptr_expr& node) -> type_name
{
    push_value(com.code(), op::push_nullptr);
    return nullptr_type();
}

auto push_expr_val(compiler& com, const node_binary_op_expr& node) -> type_name
{
    using tt = token_type;
    auto lhs = push_expr_val(com, *node.lhs);
    auto rhs = push_expr_val(com, *node.rhs);
    auto lhs_real = lhs.remove_const();
    auto rhs_real = rhs.remove_const();

    // Pointers can compare to nullptr
    if ((lhs_real.is_ptr() && rhs_real == nullptr_type()) || (rhs_real.is_ptr() && lhs_real == nullptr_type())) {
        switch (node.token.type) {
            case tt::equal_equal: { push_value(com.code(), op::u64_eq); return bool_type(); }
            case tt::bang_equal:  { push_value(com.code(), op::u64_ne); return bool_type(); }
        }
        node.token.error("could not find op '{} {} {}'", lhs, node.token.type, rhs);
    }

    if (lhs_real != rhs_real) node.token.error("could not find op '{} {} {}'", lhs, node.token.type, rhs);
    const auto& type = lhs_real;

    if (type.is_ptr()) {
        switch (node.token.type) {
            case tt::equal_equal: { push_value(com.code(), op::u64_eq); return bool_type(); }
            case tt::bang_equal:  { push_value(com.code(), op::u64_ne); return bool_type(); }
        }
    }
    else if (type == char_type()) {
        switch (node.token.type) {
            case tt::equal_equal: { push_value(com.code(), op::char_eq); return bool_type(); }
            case tt::bang_equal:  { push_value(com.code(), op::char_ne); return bool_type(); }
        }
    }
    else if (type == i32_type()) {
        switch (node.token.type) {
            case tt::plus:          { push_value(com.code(), op::i32_add); return type;       }
            case tt::minus:         { push_value(com.code(), op::i32_sub); return type;       }
            case tt::star:          { push_value(com.code(), op::i32_mul); return type;       }
            case tt::slash:         { push_value(com.code(), op::i32_div); return type;       }
            case tt::percent:       { push_value(com.code(), op::i32_mod); return type;       }
            case tt::equal_equal:   { push_value(com.code(), op::i32_eq); return bool_type(); }
            case tt::bang_equal:    { push_value(com.code(), op::i32_ne); return bool_type(); }
            case tt::less:          { push_value(com.code(), op::i32_lt); return bool_type(); }
            case tt::less_equal:    { push_value(com.code(), op::i32_le); return bool_type(); }
            case tt::greater:       { push_value(com.code(), op::i32_gt); return bool_type(); }
            case tt::greater_equal: { push_value(com.code(), op::i32_ge); return bool_type(); }
        }
    }
    else if (type == i64_type()) {
        switch (node.token.type) {
            case tt::plus:          { push_value(com.code(), op::i64_add); return type;       }
            case tt::minus:         { push_value(com.code(), op::i64_sub); return type;       }
            case tt::star:          { push_value(com.code(), op::i64_mul); return type;       }
            case tt::slash:         { push_value(com.code(), op::i64_div); return type;       }
            case tt::percent:       { push_value(com.code(), op::i64_mod); return type;       }
            case tt::equal_equal:   { push_value(com.code(), op::i64_eq); return bool_type(); }
            case tt::bang_equal:    { push_value(com.code(), op::i64_ne); return bool_type(); }
            case tt::less:          { push_value(com.code(), op::i64_lt); return bool_type(); }
            case tt::less_equal:    { push_value(com.code(), op::i64_le); return bool_type(); }
            case tt::greater:       { push_value(com.code(), op::i64_gt); return bool_type(); }
            case tt::greater_equal: { push_value(com.code(), op::i64_ge); return bool_type(); }
        }
    }
    else if (type == u64_type()) {
        switch (node.token.type) {
            case tt::plus:          { push_value(com.code(), op::u64_add); return type;       }
            case tt::minus:         { push_value(com.code(), op::u64_sub); return type;       }
            case tt::star:          { push_value(com.code(), op::u64_mul); return type;       }
            case tt::slash:         { push_value(com.code(), op::u64_div); return type;       }
            case tt::percent:       { push_value(com.code(), op::u64_mod); return type;       }
            case tt::equal_equal:   { push_value(com.code(), op::u64_eq); return bool_type(); }
            case tt::bang_equal:    { push_value(com.code(), op::u64_ne); return bool_type(); }
            case tt::less:          { push_value(com.code(), op::u64_lt); return bool_type(); }
            case tt::less_equal:    { push_value(com.code(), op::u64_le); return bool_type(); }
            case tt::greater:       { push_value(com.code(), op::u64_gt); return bool_type(); }
            case tt::greater_equal: { push_value(com.code(), op::u64_ge); return bool_type(); }
        }
    }
    else if (type == f64_type()) {
        switch (node.token.type) {
            case tt::plus:          { push_value(com.code(), op::f64_add); return type;       }
            case tt::minus:         { push_value(com.code(), op::f64_sub); return type;       }
            case tt::star:          { push_value(com.code(), op::f64_mul); return type;       }
            case tt::slash:         { push_value(com.code(), op::f64_div); return type;       }
            case tt::equal_equal:   { push_value(com.code(), op::f64_eq); return bool_type(); }
            case tt::bang_equal:    { push_value(com.code(), op::f64_ne); return bool_type(); }
            case tt::less:          { push_value(com.code(), op::f64_lt); return bool_type(); }
            case tt::less_equal:    { push_value(com.code(), op::f64_le); return bool_type(); }
            case tt::greater:       { push_value(com.code(), op::f64_gt); return bool_type(); }
            case tt::greater_equal: { push_value(com.code(), op::f64_ge); return bool_type(); }
        }
    }
    else if (type == bool_type()) {
        switch (node.token.type) {
            case tt::ampersand_ampersand: { push_value(com.code(), op::bool_and); return type; }
            case tt::bar_bar:             { push_value(com.code(), op::bool_or);  return type; }
            case tt::equal_equal:         { push_value(com.code(), op::bool_eq);  return type; }
            case tt::bang_equal:          { push_value(com.code(), op::bool_ne);  return type; }
        }
    }

    node.token.error("could not find op '{} {} {}'", lhs, node.token.type, rhs);
}

auto push_expr_val(compiler& com, const node_unary_op_expr& node) -> type_name
{
    using tt = token_type;
    const auto raw_type = push_expr_val(com, *node.expr);
    const auto type = raw_type.remove_const();

    switch (node.token.type) {
        case tt::minus: {
            if (type == i32_type()) { push_value(com.code(), op::i32_neg); return type; }
            if (type == i64_type()) { push_value(com.code(), op::i64_neg); return type; }
            if (type == f64_type()) { push_value(com.code(), op::f64_neg); return type; }
        } break;
        case tt::bang: {
            if (type == bool_type()) { push_value(com.code(), op::bool_not); return type; }
        } break;
    }
    node.token.error("could not find op '{}{}'", node.token.type, type);
}

// This is also used for declaration and assignment, should probably be renamed.
auto push_function_arg(
    compiler& com, const node_expr& expr, const type_name& expected_raw, const token& tok
) -> void
{
    // Can disregard constness since the argument is getting copied anyway.
    const auto actual = type_of_expr(com, expr).remove_const();
    const auto expected = expected_raw.remove_const();

    if (actual.is_arena() || expected.is_arena()) {
        tok.error("arenas can not be copied or assigned");
    }

    const auto exact_match = actual == expected;

    // T& can be assigned to a (const T)&
    const auto ptr_convertible = actual.is_ptr() &&
                                 expected.is_ptr() &&
                                 actual.remove_ptr().add_const() == expected.remove_ptr();

    // T[] can be assigned to a (const T)[]
    const auto span_convertible = actual.is_span() &&
                                  expected.is_span() &&
                                  actual.remove_span().add_const() == expected.remove_span();

    // nullptr can be assigned to any pointer
    const auto nullptr_to_ptr = expected.is_ptr() && actual == nullptr_type();

    if (exact_match || ptr_convertible || span_convertible || nullptr_to_ptr) {
        push_expr_val(com, expr);
    } else {
        tok.error("Cannot convert '{}' to '{}'", actual, expected);
    }
}

auto get_builtin_id(const std::string& name)
    -> std::optional<std::size_t>
{
    auto index = std::size_t{0};
    for (const auto& b : get_builtins()) {
        if (name == b.name) {
            return index;
        }
        ++index;
    }
    return std::nullopt;
}

auto push_expr_val(compiler& com, const node_call_expr& node) -> type_name
{
    // First, handle the cases where the thing we are trying to call is a name.
    if (std::holds_alternative<node_name_expr>(*node.expr)) {
        auto& inner = std::get<node_name_expr>(*node.expr);

        // First, it might be a constructor call
        const auto type = make_type(inner.name);
        if (inner.struct_name == nullptr && com.types.contains(type)) {
            const auto expected_params = get_constructor_params(com, type);
            node.token.assert_eq(expected_params.size(), node.args.size(),
                                 "bad number of arguments to constructor call");
            for (std::size_t i = 0; i != node.args.size(); ++i) {
                push_function_arg(com, *node.args.at(i), expected_params[i], node.token);
            }
            if (node.args.size() == 0) { // if the class has no data, it needs to be size 1
                push_value(com.code(), op::push_null);
            }
            return type;
        }

        // Hack to allow for an easy way to dump types of expressions
        if (inner.struct_name == nullptr & inner.name == "__dump_type") {
            std::print("__dump_type(\n");
            for (const auto& arg : node.args) {
                const auto dump = type_of_expr(com, *arg);
                std::print("    {},\n", dump);
            }
            std::print(")\n");
            push_value(com.code(), op::push_null);
            return null_type();
        }

        // Second, it might be a function call
        const auto struct_type = resolve_type(com, node.token, inner.struct_name);
        if (const auto func = get_function(com, to_string(struct_type), inner.name); func) {
            node.token.assert_eq(node.args.size(), func->sig.params.size(), "bad number of arguments to function call");
            for (std::size_t i = 0; i != node.args.size(); ++i) {
                push_function_arg(com, *node.args.at(i), func->sig.params[i], node.token);
            }
            push_function_call(com, *func);
            return func->sig.return_type;
        }

        // Lastly, it might be a builtin function
        // TODO- fix type checking
        if (const auto b = get_builtin_id(inner.name); b.has_value()) {
            const auto& builtin = get_builtin(*b);
            node.token.assert_eq(node.args.size(), builtin.args.size(), "bad number of arguments to builtin call");
            for (std::size_t i = 0; i != builtin.args.size(); ++i) {
                push_function_arg(com, *node.args.at(i), builtin.args[i], node.token);
            }
            push_value(com.code(), op::builtin_call, *b);
            return get_builtin(*b).return_type;
        }
    }

    // Otherwise, the expression must be a function pointer.
    const auto type = type_of_expr(com, *node.expr).remove_const();
    node.token.assert(type.is_function_ptr(), "unable to call non-callable type {}", type);

    const auto& sig = std::get<type_function_ptr>(type);

    auto args_size = std::size_t{0};
    for (std::size_t i = 0; i != node.args.size(); ++i) {
        push_function_arg(com, *node.args.at(i), sig.param_types[i], node.token);
        args_size += com.types.size_of(sig.param_types[i]);
    }

    // push the function pointer and call it
    push_expr_val(com, *node.expr);
    push_value(com.code(), op::call, args_size);
    return *sig.return_type;
}

// TODO- Allow member call through a pointer
auto push_expr_val(compiler& com, const node_member_call_expr& node) -> type_name
{
    const auto [type, is_const] = type_of_expr(com, *node.expr).strip_const(); 

    // Handle .size() calls on arrays
    if (type.is_array() && node.function_name == "size") {
        node.token.assert(node.other_args.empty(), "{}.size() takes no extra arguments", type);
        push_value(com.code(), op::push_u64, array_length(type));
        return u64_type();
    }

    // Handle .size() calls on spans
    if (type.is_span() && node.function_name == "size") {
        node.token.assert(node.other_args.empty(), "{}.size() takes no extra arguments", type);
        push_expr_ptr(com, *node.expr); // push pointer to span
        push_value(com.code(), op::push_u64, sizeof(std::byte*));
        push_value(com.code(), op::u64_add); // offset to the size value
        push_value(com.code(), op::load, com.types.size_of(u64_type())); // load the size
        return u64_type();
    }

    // Handle arena functions
    if (type.is_arena()) {
        if (node.function_name == "new") {
            if (!node.template_type) node.token.error("calls to arena 'create' must have a template type");
            const auto result_type = resolve_type(com, node.token, node.template_type);
            
            // First, build the object on the stack
            const auto expected_params = get_constructor_params(com, result_type);
            node.token.assert_eq(expected_params.size(), node.other_args.size(),
                                "incorrect number of arguments to constructor call");
            for (std::size_t i = 0; i != node.other_args.size(); ++i) {
                push_function_arg(com, *node.other_args.at(i), expected_params[i], node.token);
            }
            if (node.other_args.size() == 0) { // if the class has no data, it needs to be size 1
                push_value(com.code(), op::push_null);
            }
            
            // Allocate space in the arena and move the object there
            // (the allocate op code will do the move)
            const auto size = com.types.size_of(result_type);
            push_expr_val(com, *node.expr); // push the value of the arena, which is a pointer to the C++ struct
            push_value(com.code(), op::arena_alloc, size);
            return result_type.add_ptr();
        }
        else if (node.function_name == "new_array") {
            if (!node.template_type) node.token.error("calls to arena 'create' must have a template type");
            const auto result_type = resolve_type(com, node.token, node.template_type);
            
            // First, push the count onto the stack
            const auto expected_params = std::vector<type_name>{u64_type()};
            node.token.assert_eq(expected_params.size(), node.other_args.size(),
                                "incorrect number of arguments to array constructor call");
            for (std::size_t i = 0; i != node.other_args.size(); ++i) {
                push_function_arg(com, *node.other_args.at(i), expected_params[i], node.token);
            }
            
            // Allocate space in the arena and move the object there
            // (the allocate op code will do the move)
            const auto size = com.types.size_of(result_type);
            push_expr_val(com, *node.expr); // push the value of the arena, which is a pointer to the C++ struct
            push_value(com.code(), op::arena_alloc_array, size);
            return result_type.add_span();
        }
        else if (node.function_name == "size") {
            push_expr_val(com, *node.expr); // push the value of the arena, which is a pointer to the C++ struct
            push_value(com.code(), op::arena_size);
            return u64_type();
        }
        else if (node.function_name == "capacity") {
            push_expr_val(com, *node.expr); // push the value of the arena, which is a pointer to the C++ struct
            push_value(com.code(), op::arena_capacity);
            return u64_type();
        }
        else {
            node.token.error("Unknown arena function '{}'\n", node.function_name);
        }
    }

    const auto stripped_type = [&] {
        auto t = type;
        while (t.is_ptr()) { t = t.remove_ptr(); }
        return t;
    }();

    auto params = std::vector<type_name>{};
    params.push_back(stripped_type.add_ptr());
    for (const auto& arg : node.other_args) {
        params.push_back(type_of_expr(com, *arg));
    }

    const auto func = get_function(com, to_string(stripped_type), node.function_name);
    node.token.assert(func.has_value(), "could not find member function {}::{}", stripped_type, node.function_name);
    
    // We wrap the LHS in a addrof so that we can use push_function_arg to push it
    // like a regular function arg.
    auto synthetic_node = std::make_shared<node_expr>();
    auto& inner = synthetic_node->emplace<node_addrof_expr>();
    inner.expr = node.expr;
    inner.token = node.token;

    push_function_arg(com, *synthetic_node, func->sig.params[0], node.token);
    auto t = type;
    while (t.is_ptr()) { // allow for calling member functions through pointers
        push_value(com.code(), op::load, sizeof(std::byte*));
        t = t.remove_ptr();
    }
    for (std::size_t i = 0; i != node.other_args.size(); ++i) {
        push_function_arg(com, *node.other_args.at(i), func->sig.params[i + 1], node.token);
    }
    push_function_call(com, *func);
    return func->sig.return_type;
}

auto push_expr_val(compiler& com, const node_array_expr& node) -> type_name
{
    node.token.assert(!node.elements.empty(), "cannot have empty array literals");

    const auto inner_type = push_expr_val(com, *node.elements.front());
    for (const auto& element : node.elements | std::views::drop(1)) {
        const auto element_type = push_expr_val(com, *element);
        node.token.assert_eq(element_type, inner_type, "array has mismatching element types");
    }
    return inner_type.add_array(node.elements.size());
}

auto push_expr_val(compiler& com, const node_repeat_array_expr& node) -> type_name
{
    node.token.assert(node.size != 0, "cannot have empty array literals");

    const auto inner_type = type_of_expr(com, *node.value);
    for (std::size_t i = 0; i != node.size; ++i) {
        push_expr_val(com, *node.value);
    }
    return inner_type.add_array(node.size);
}

auto push_expr_val(compiler& com, const node_addrof_expr& node) -> type_name
{
    const auto type = push_expr_ptr(com, *node.expr);
    return type.add_ptr();
}

auto push_expr_val(compiler& com, const node_sizeof_expr& node) -> type_name
{
    const auto type = type_of_expr(com, *node.expr);
    push_value(com.code(), op::push_u64, com.types.size_of(type));
    return u64_type();
}

auto push_expr_val(compiler& com, const node_span_expr& node) -> type_name
{
    if ((node.lower_bound && !node.upper_bound) || (!node.lower_bound && node.upper_bound)) {
        node.token.error("a span must either have both bounds set, or neither");
    }

    const auto [type, is_const] = type_of_expr(com, *node.expr).strip_const();
    node.token.assert(
        type.is_array() || type.is_span(),
        "can only span arrays and other spans, not {}", type
    );

    push_expr_ptr(com, *node.expr);

    // If we are a span, we want the address that it holds rather than its own address,
    // so switch the pointer by loading what it's pointing at.
    if (type.is_span()) {
        push_value(com.code(), op::load, sizeof(std::byte*));
    }

    if (node.lower_bound) {// move first index of span up
        push_value(com.code(), op::push_u64, com.types.size_of(inner_type(type)));
        const auto lower_bound_type = push_expr_val(com, *node.lower_bound);
        node.token.assert_eq(lower_bound_type, u64_type(), "subspan indices must be u64");
        push_value(com.code(), op::u64_mul);
        push_value(com.code(), op::u64_add);
    }

    // next push the size to make up the second half of the span
    if (node.lower_bound && node.upper_bound) {
        push_expr_val(com, *node.upper_bound);
        push_expr_val(com, *node.lower_bound);
        push_value(com.code(), op::u64_sub);
    } else if (type.is_span()) {
        // Push the span pointer, offset to the size, and load the size
        push_expr_ptr(com, *node.expr);
        push_value(com.code(), op::push_u64, sizeof(std::byte*), op::u64_add);
        push_value(com.code(), op::load, com.types.size_of(u64_type()));
    } else {
        push_value(com.code(), op::push_u64, array_length(type));
    }

    if (is_const && type.is_array()) {
        return type.remove_array().add_const().add_span();
    }
    return type.remove_array().add_span();
}

// If not implemented explicitly, assume that the given node_expr is an lvalue, in which case
// we can load it by pushing the address to the stack and loading.
auto push_expr_val(compiler& com, const auto& node) -> type_name
{
    const auto type = push_expr_ptr(com, node);
    push_value(com.code(), op::load, com.types.size_of(type));
    return type;
}

void push_stmt(compiler& com, const node_sequence_stmt& node)
{
    const auto scope = com.variables.new_scope();
    for (const auto& seq_node : node.sequence) {
        push_stmt(com, *seq_node);
    }
}

auto push_loop(compiler& com, std::function<void()> body) -> void
{
    const auto loop_scope = com.variables.new_loop_scope();
    
    const auto begin_pos = com.code().size();
    {
        const auto body_scope = com.variables.new_scope();
        body();
    }
    push_value(com.code(), op::jump, begin_pos);

    // Fix up the breaks and continues
    const auto& control_flow = com.variables.get_loop_info();
    for (const auto idx : control_flow.breaks) {
        write_value(com.code(), idx, com.code().size()); // Jump past end
    }
    for (const auto idx : control_flow.continues) {
        write_value(com.code(), idx, begin_pos); // Jump to start
    }
}

void push_stmt(compiler& com, const node_loop_stmt& node)
{
    push_loop(com, [&] {
        push_stmt(com, *node.body);
    });
}

void push_break(compiler& com, const token& tok)
{
    tok.assert(com.variables.in_loop(), "cannot use 'break' outside of a loop");
    com.variables.handle_loop_exit();
    push_value(com.code(), op::jump);
    const auto pos = push_value(com.code(), std::uint64_t{0}); // filled in later
    com.variables.get_loop_info().breaks.push_back(pos);
}

/*
while <condition> {
    <body>
}

becomes

loop {
    if !<condition> break;
    <body>
}
*/
void push_stmt(compiler& com, const node_while_stmt& node)
{
    push_loop(com, [&] {
        // if !<condition> break;
        const auto cond_type = push_expr_val(com, *node.condition);
        node.token.assert_eq(cond_type, bool_type(), "while-stmt invalid condition");
        push_value(com.code(), op::bool_not);
        push_value(com.code(), op::jump_if_false);
        const auto jump_pos = push_value(com.code(), std::uint64_t{0});
        push_break(com, node.token);
        write_value(com.code(), jump_pos, com.code().size()); // Jump past the end if false      
        
        // <body>
        push_stmt(com, *node.body);
    });
}

/*
for <name> in <iter> {
    <body>
}

becomes

{
    <<create temporary var if iter is an rvalue>>
    idx = 0u;
    size := <<length of iter>>;
    loop {
        if idx == size break;
        name := iter[idx]~;
        idx = idx + 1u;
        <body>
    }
}
*/
void push_stmt(compiler& com, const node_for_stmt& node)
{
    const auto scope = com.variables.new_scope();

    const auto iter_type = type_of_expr(com, *node.iter);

    const auto is_array = iter_type.is_array();
    const auto is_lvalue_span = iter_type.is_span() && is_lvalue_expr(*node.iter);
    node.token.assert(is_array || is_lvalue_span, "for-loops only supported for arrays and lvalue spans");

    // Need to create a temporary if we're using an rvalue
    if (is_rvalue_expr(*node.iter)) {
        push_expr_val(com, *node.iter);
        declare_var(com, node.token, "#:iter", iter_type);
    }

    // idx := 0u;
    push_value(com.code(), op::push_u64, std::uint64_t{0});
    declare_var(com, node.token, "#:idx", u64_type());

    // size := length of iter;
    if (iter_type.is_array()) {
        push_value(com.code(), op::push_u64, array_length(iter_type));
        declare_var(com, node.token, "#:size", u64_type());
    } else {
        node.token.assert(is_lvalue_expr(*node.iter), "for-loops only supported for lvalue spans");
        push_expr_ptr(com, *node.iter); // push pointer to span
        push_value(com.code(), op::push_u64, sizeof(std::byte*));
        push_value(com.code(), op::u64_add); // offset to the size value
        push_value(com.code(), op::load, com.types.size_of(u64_type()));       
        declare_var(com, node.token, "#:size", u64_type());
    }

    push_loop(com, [&] {
        // if idx == size break;
        load_variable(com, node.token, "#:idx");
        load_variable(com, node.token, "#:size");
        push_value(com.code(), op::u64_eq);
        push_value(com.code(), op::jump_if_false);
        const auto jump_pos = push_value(com.code(), std::uint64_t{0});
        push_break(com, node.token);
        write_value(com.code(), jump_pos, com.code().size());

        // name := iter[idx]~;
        const auto iter_type = type_of_expr(com, *node.iter);
        const auto inner = inner_type(iter_type);
        if (is_rvalue_expr(*node.iter)) {
            push_var_addr(com, node.token, "#:iter");
        } else {
            push_expr_ptr(com, *node.iter);
            if (iter_type.is_span()) {
                push_value(com.code(), op::load, sizeof(std::byte*));
            }
        }
        load_variable(com, node.token, "#:idx");
        push_value(com.code(), op::push_u64, com.types.size_of(inner));
        push_value(com.code(), op::u64_mul);
        push_value(com.code(), op::u64_add);
        declare_var(com, node.token, node.name, inner.add_ptr());

        // idx = idx + 1;
        load_variable(com, node.token, "#:idx");
        push_value(com.code(), op::push_u64, std::uint64_t{1}, op::u64_add);
        save_variable(com, node.token, "#:idx");

        // main body
        push_stmt(com, *node.body);
    });
}

void push_stmt(compiler& com, const node_if_stmt& node)
{
    const auto cond_type = push_expr_val(com, *node.condition);
    node.token.assert_eq(cond_type, bool_type(), "if-stmt invalid condition");

    push_value(com.code(), op::jump_if_false);
    const auto jump_pos = push_value(com.code(), std::uint64_t{0});
    push_stmt(com, *node.body);

    if (node.else_body) {
        push_value(com.code(), op::jump);
        const auto else_pos = push_value(com.code(), std::uint64_t{0});
        const auto in_else_pos = com.code().size();
        push_stmt(com, *node.else_body);
        write_value(com.code(), jump_pos, in_else_pos); // Jump into the else block if false
        write_value(com.code(), else_pos, com.code().size()); // Jump past the end if false
    } else {
        write_value(com.code(), jump_pos, com.code().size()); // Jump past the end if false
    }
}

void push_stmt(compiler& com, const node_struct_stmt& node)
{
    const auto message = std::format("type '{}' already defined", node.name);
    node.token.assert(!com.types.contains(make_type(node.name)), "{}", message);
    node.token.assert(!com.functions_by_name.contains(node.name), "{}", message);

    auto fields = std::vector<type_field>{};
    for (const auto& p : node.fields) {
        fields.emplace_back(type_field{ .name=p.name, .type=resolve_type(com, node.token, p.type) });
    }

    com.types.add(make_type(node.name), fields);
    for (const auto& function : node.functions) {
        push_stmt(com, *function);
    }
}

void push_stmt(compiler& com, const node_break_stmt& node)
{
    push_break(com, node.token);
}

void push_stmt(compiler& com, const node_continue_stmt& node)
{
    node.token.assert(com.variables.in_loop(), "cannot use 'continue' outside of a loop");
    com.variables.handle_loop_exit();
    push_value(com.code(), op::jump);
    const auto pos = push_value(com.code(), std::uint64_t{0}); // filled in later
    com.variables.get_loop_info().continues.push_back(pos);
}

auto push_stmt(compiler& com, const node_declaration_stmt& node) -> void
{
    const auto type = node.explicit_type ? resolve_type(com, node.token, node.explicit_type)
                                         : type_of_expr(com, *node.expr).remove_const();
    node.token.assert(!type.is_arena(), "cannot create copies of arenas");
    push_function_arg(com, *node.expr, type, node.token);
    declare_var(com, node.token, node.name, node.add_const ? type.add_const() : type);
}

auto push_stmt(compiler& com, const node_arena_declaration_stmt& node) -> void
{
    const auto type = arena_type();
    push_value(com.code(), op::arena_new);
    declare_var(com, node.token, node.name, type);
}

void push_stmt(compiler& com, const node_assignment_stmt& node)
{
    const auto lhs_type = type_of_expr(com, *node.position);
    node.token.assert(!lhs_type.is_const(), "cannot assign to a const variable");
    push_function_arg(com, *node.expr, lhs_type, node.token);
    const auto lhs = push_expr_ptr(com, *node.position);
    push_value(com.code(), op::save, com.types.size_of(lhs));
    return;
}

auto ends_in_return(const node_stmt& node) -> bool
{
    return std::visit(overloaded{
        [&](const node_sequence_stmt& n) {
            if (n.sequence.empty()) { return false; }
            return ends_in_return(*n.sequence.back());
        },
        [&](const node_if_stmt& n) {
            if (!n.else_body) { return false; } // both branches must exist
            return ends_in_return(*n.body) && ends_in_return(*n.else_body);
        },
        [](const node_return_stmt&) { return true; },
        [](const auto&)             { return false; }
    }, node);
}

auto compile_function_body(
    compiler& com,
    const token& tok,
    const type_name& struct_type,
    const std::string& name,
    const node_signature& node_sig,
    const node_stmt_ptr& body
)
    -> void
{
    new_function(com, std::format("{}::{}", struct_type, name), tok);
    {
        const auto scope = com.variables.new_function_scope(null_type());

        for (const auto& arg : node_sig.params) {
            const auto type = resolve_type(com, tok, arg.type);
            declare_var(com, tok, arg.name, type);
            com.current().sig.params.push_back(type);
        }
        com.current().sig.return_type = resolve_type(com, tok, node_sig.return_type);

        push_stmt(com, *body);

        if (!ends_in_return(*body)) {
            // A function returning null does not need a final return statement, and in this case
            // we manually add a return value of null here.
            if (com.current().sig.return_type == null_type()) {
                push_value(com.code(), op::push_null);
                push_value(com.code(), op::ret, std::uint64_t{1});
            } else {
                tok.error("function '{}::{}' does not end in a return statement", struct_type, name);
            }
        }
    }
    finish_function(com);
}

void push_stmt(compiler& com, const node_function_def_stmt& node)
{
    if (com.types.contains(make_type(node.name))) {
        node.token.error("'{}' cannot be a function name, it is a type def", node.name);
    }
    compile_function_body(com, node.token, global_namespace, node.name, node.sig, node.body);
}

void push_stmt(compiler& com, const node_member_function_def_stmt& node)
{
    const auto struct_type = make_type(node.struct_name);

    // First argument must be a pointer to an instance of the class
    node.token.assert(node.sig.params.size() > 0, "member functions must have at least one arg");
    const auto actual = resolve_type(com, node.token, node.sig.params[0].type);
    const auto expected = struct_type.add_ptr();
    const auto const_expected = struct_type.add_const().add_ptr();
    
    node.token.assert(
        actual == expected || actual == const_expected,
        "first parameter to a struct member function must be a pointer to that type, "
        "expected '{}' or '{}', got '{}'",
        expected, const_expected, actual
    );

    compile_function_body(com, node.token, struct_type, node.function_name, node.sig, node.body);
}

void push_stmt(compiler& com, const node_return_stmt& node)
{
    node.token.assert(com.in_function, "can only return within functions");
    const auto return_type = push_expr_val(com, *node.return_value);
    node.token.assert_eq(
        return_type.remove_const(), // don't impose const on the return value
        com.current().sig.return_type,
        "wrong return type"
    );
    com.variables.handle_function_exit();
    push_value(com.code(), op::ret, com.types.size_of(return_type));
}

void push_stmt(compiler& com, const node_expression_stmt& node)
{
    const auto type = push_expr_val(com, *node.expr);
    push_value(com.code(), op::pop, com.types.size_of(type));
}

void push_stmt(compiler& com, const node_assert_stmt& node)
{
    const auto expr = type_of_expr(com, *node.expr);
    node.token.assert_eq(expr, bool_type(), "bad assertion expression");
    push_expr_val(com, *node.expr);
    push_assert(com, std::format("line {}", node.token.line));
}

// Temp: remove this for a more efficient function
auto string_replace(
    std::string subject, std::string_view search, std::string_view replace
)
    -> std::string
{
    std::size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos) {
         subject.replace(pos, search.length(), replace);
         pos += replace.length();
    }
    return subject;
}

// Temp: remove this for a more efficient function
auto string_split(std::string s, std::string_view delimiter) -> std::vector<std::string>
{
    std::size_t pos_start = 0;
    std::size_t pos_end = 0;
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
        token = s.substr(pos_start, pos_end - pos_start);
        pos_start = pos_end + delimiter.length();
        res.push_back(token);
    }

    res.push_back(s.substr(pos_start));
    return res;
}

auto push_print_fundamental(compiler& com, const node_expr& node, const token& tok) -> void
{
    const auto type = push_expr_val(com, node).remove_const();
    if (type == null_type()) { push_value(com.code(), op::print_null); }
    else if (type == bool_type()) { push_value(com.code(), op::print_bool); }
    else if (type == char_type()) { push_value(com.code(), op::print_char); }
    else if (type == i32_type()) { push_value(com.code(), op::print_i32); }
    else if (type == i64_type()) { push_value(com.code(), op::print_i64); }
    else if (type == u64_type()) { push_value(com.code(), op::print_u64); }
    else if (type == f64_type()) { push_value(com.code(), op::print_f64); }
    else if (type == char_type().add_const().add_span() || type == char_type().add_span()) {
        push_value(com.code(), op::print_char_span);
    }
    else if (type == nullptr_type()) { push_value(com.code(), op::print_ptr); }
    else if (type.is_ptr()) { push_value(com.code(), op::print_ptr); }
    else { tok.error("cannot print value of type {}", type); }
}

void push_stmt(compiler& com, const node_print_stmt& node)
{
    const auto parts = string_split(string_replace(node.message, "\\n", "\n"), "{}");
    if (parts.size() != node.args.size() + 1) {
        node.token.error("Not enough args to fill all placeholders");
    }

    if (!parts.front().empty()) {
        push_value(com.code(), op::push_string_literal);
        push_value(com.code(), insert_into_rom(com, parts.front()), parts.front().size());
        push_value(com.code(), op::print_char_span);
    }
    for (std::size_t i = 0; i != node.args.size(); ++i) {
        push_print_fundamental(com, *node.args.at(i), node.token);

        if (!parts[i+1].empty()) {
            push_value(com.code(), op::push_string_literal);
            push_value(com.code(), insert_into_rom(com, parts[i+1]), parts[i+1].size());
            push_value(com.code(), op::print_char_span);
        }
    }
}

auto push_expr_val(compiler& com, const node_expr& expr) -> type_name
{
    return std::visit([&](const auto& node) { return push_expr_val(com, node); }, expr);
}

auto push_stmt(compiler& com, const node_stmt& root) -> void
{
    std::visit([&](const auto& node) { push_stmt(com, node); }, root);
}

}

auto compile(const anzu_module& ast) -> bytecode_program
{
    auto com = compiler{};
    com.variables.set_compiler(com);
    new_function(com, "$main", token{});
    com.in_function = false; // the outer function is not a real function

    {
        const auto global_scope = com.variables.new_scope();
        push_stmt(com, *ast.root);
    }

    push_value(com.code(), op::end_program);

    auto program = bytecode_program{};
    program.rom = com.rom;
    for (const auto& function : com.compiled_functions) {
        program.functions.push_back(bytecode_function{function.name, function.id, function.code});
    }
    return program;
}

}