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

// Returns the current function
auto current(compiler& com) -> function& {
    return com.functions[com.current_function.back().id];
}

// Returns the bytecode that we are currently writing to
auto code(compiler& com) -> std::vector<std::byte>& {
    return current(com).code;
}

auto in_function(compiler& com) -> bool {
    return com.current_function.size() > 1;
}

// Access local variables if in a function, otherwise the globals
auto variables(compiler& com) -> variable_manager& {
    return current(com).variables;
}

auto globals(compiler& com) -> variable_manager& {
    return com.functions.front().variables;
}

static const auto global_namespace = type_name{type_struct{""}};
enum class compile_type { val, ptr };

auto push_expr(compiler& com, compile_type ct, const node_expr& node) -> type_name;
auto push_stmt(compiler& com, const node_stmt& root) -> void;
auto type_of_expr(compiler& com, const node_expr& node) -> type_name;

auto make_type(compiler& com, const std::string& name) -> type_name
{
    // First check the current function templates
    const auto& map1 = com.current_function.back().templates;
    if (auto it = map1.find(name); it != map1.end()) return it->second;

    // Then check the current struct templates
    const auto& map2 = com.current_struct.back().templates;
    if (auto it = map2.find(name); it != map2.end()) return it->second;

    if (name == "null") return type_name(type_fundamental::null_type);
    if (name == "bool") return type_name(type_fundamental::bool_type);
    if (name == "char") return type_name(type_fundamental::char_type);
    if (name == "i32") return  type_name(type_fundamental::i32_type);
    if (name == "i64") return  type_name(type_fundamental::i64_type);
    if (name == "u64") return  type_name(type_fundamental::u64_type);
    if (name == "f64") return  type_name(type_fundamental::f64_type);
    if (name == "nullptr") return type_name(type_fundamental::nullptr_type);
    if (name == "arena") return type_name(type_arena{});
    return type_struct{ .name=name };
}

// If the given expression results in a type expression, return the inner type.
// Otherwise program is ill-formed.
auto resolve_type(compiler& com, const token& tok, const node_expr_ptr& expr) -> type_name
{
    const auto type_expr_type = type_of_expr(com, *expr);
    
    // null and nullptr and also their own types
    if (type_expr_type == null_type() || type_expr_type == nullptr_type()) {
        return type_expr_type;
    }
    
    tok.assert(type_expr_type.is_type_value(), "expected type expression, got {}", type_expr_type);
    return inner_type(type_expr_type);
}

auto struct_name(
    compiler& com, const token& tok, const std::string& name, const std::vector<node_expr_ptr>& templates = {}
)
    -> type_name
{
    if (templates.empty()) {
        return make_type(com, name);
    }
    const auto template_str = format_comma_separated(
        templates, [&](const node_expr_ptr& n) { return resolve_type(com, tok, n); }
    );
    return make_type(com, std::format("{}!({})", name, template_str));
}

auto fn_name(
    compiler& com,
    const token& tok,
    const type_name& struct_name,
    const std::string& function_name,
    const std::vector<node_expr_ptr>& template_args = {}
)
    -> std::string
{
    auto name = std::string{};
    if (struct_name != global_namespace) {
        name += std::format("{}::", struct_name.remove_const());
    }
    name += function_name;

    if (!template_args.empty()) {
        const auto template_args_string = format_comma_separated(
            template_args, [&](const node_expr_ptr& n) { return resolve_type(com, tok, n); }
        );
        name += std::format("!({})", template_args_string);
    }

    return name;
}

auto get_function(compiler& com, const std::string& full_name) -> std::optional<function>
{
    if (const auto it = com.functions_by_name.find(full_name); it != com.functions_by_name.end()) {
        return com.functions[it->second];
    }
    return std::nullopt;
}

// Registers the given name in the current scope
void declare_var(compiler& com, const token& tok, const std::string& name, const type_name& type)
{
    if (!current(com).variables.declare(name, type, com.types.size_of(type))) {
        tok.error("name already in use: '{}'", name);
    }
}

auto push_var_addr(compiler& com, const token& tok, const std::string& name) -> type_name
{
    if (in_function(com)) {
        if (const auto var = variables(com).find(name); var.has_value()) {
            push_value(code(com), op::push_ptr_local, var->location);
            return var->type;
        }
    }

    const auto var = globals(com).find(name);
    tok.assert(var.has_value(), "could not find variable '{}'\n", name);
    push_value(code(com), op::push_ptr_global, var->location);
    return var->type;
}

auto load_variable(compiler& com, const token& tok, const std::string& name) -> void
{
    const auto type = push_var_addr(com, tok, name);
    push_value(code(com), op::load, com.types.size_of(type));
}

auto save_variable(compiler& com, const token& tok, const std::string& name) -> void
{
    const auto type = push_var_addr(com, tok, name);
    push_value(code(com), op::save, com.types.size_of(type));
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
            push_value(code(com), op::push_u64, offset);
            return field.type;
        }
        offset += com.types.size_of(field.type);
    }
    
    tok.error("could not find field '{}' for type '{}'\n", field_name, type.remove_const());
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
    const auto program_size = code(com).size();
    const auto type = push_expr(com, compile_type::val, node);
    code(com).resize(program_size);
    return type;
}

// Fetches the given literal from read only memory, or adds it if it is not there, and
// returns the pointer.
auto insert_into_rom(compiler& com, std::string_view data) -> std::size_t
{
    if (const auto index = com.rom.find(data); index != std::string::npos) {
        return index;
    }
    const auto ptr = com.rom.size();
    com.rom.append(data);
    return ptr;
}

// Given a type, push the number of op::load calls required to dereference away all the pointers.
// If the type is not a pointer, this is a noop.
auto auto_deref_pointer(compiler& com, const type_name& type) -> type_name
{
    auto t = type;
    while (t.is_ptr()) {
        push_value(code(com), op::load, sizeof(std::byte*));
        t = t.remove_ptr();
    }
    return t;
}

auto strip_pointers(const type_name& type) -> type_name
{
    auto t = type;
    while (t.is_ptr()) t = t.remove_ptr();
    return t;
}

auto const_convertable_to(const token& tok, const type_name& src, const type_name& dst) {
    if (src.is_const && !dst.is_const) {
        return false;
    }

    return std::visit(overloaded{
        [&](type_fundamental l, type_fundamental r) { return l == r; },
        [&](const type_struct& l, const type_struct& r) { return l == r; },
        [&](const type_array& l, const type_array& r) {
            return l.count == r.count && const_convertable_to(tok, *l.inner_type, *r.inner_type);
        },
        [&](const type_ptr& l, const type_ptr& r) {
            return const_convertable_to(tok, *l.inner_type, *r.inner_type);
        },
        [&](const type_span& l, const type_span& r) {
            return const_convertable_to(tok, *l.inner_type, *r.inner_type);
        },
        [&](const type_function_ptr& l, const type_function_ptr& r) { return l == r; },
        [&](const type_builtin& l, const type_builtin& r) { return l == r; },
        [&](const type_bound_method& l, const type_bound_method& r) { return l == r; },
        [&](const type_bound_builtin_method& l, const type_bound_builtin_method& r) { return l == r; },
        [&](const type_arena& l, const type_arena& r) { return true; },
        [&](const auto& l, const auto& r) {
            return false;
        }
    }, src, dst);
}

// Used for passing copies of variables to functions, as well as for assignments and declarations.
// Verifies that the type of the expression can be converted to the type 
void push_copy_typechecked(compiler& com, const node_expr& expr, const type_name& expected_raw, const token& tok)
{
    // Remove top-level const since we are making a copy, ie- you should be able to pass a
    // 'u64 const' for a 'u64', but not a 'u64 const&' for a 'u64&' (though passing a 'u64&'
    // for a 'u64 const&' is fine)
    const auto actual = push_expr(com, compile_type::val, expr).remove_const();
    const auto expected = expected_raw.remove_const();

    if (actual == nullptr_type() && expected.is_ptr()) {
        return;
    }

    // Allow for a span to be constructed from a nullptr, which results in a null span.
    if (actual == nullptr_type() && expected.is_span()) {
        push_value(code(com), op::push_u64, std::size_t{0}); // push the size
        return;
    }

    if (actual.is_arena() || expected.is_arena()) {
        tok.error("arenas can not be copied or assigned");
    }

    if (!const_convertable_to(tok, actual, expected)) {
        tok.error("Cannot convert '{}' to '{}'", actual, expected);
    }
}

void push_break(compiler& com, const token& tok)
{
    tok.assert(variables(com).in_loop(), "cannot use 'break' outside of a loop");
    variables(com).handle_loop_exit(code(com));
    push_value(code(com), op::jump);
    const auto pos = push_value(code(com), std::uint64_t{0}); // filled in later
    variables(com).get_loop_info().breaks.push_back(pos);
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

auto build_template_map(
    compiler& com,
    const token& tok,
    const std::vector<std::string>& names,
    const std::vector<node_expr_ptr>& types
)
    -> template_map
{
    tok.assert_eq(types.size(), names.size(), "bad number of template args");
    auto map = template_map{};
    for (const auto& [actual, expected] : zip(types, names)) {
        const auto [it, success] = map.emplace(expected, resolve_type(com, tok, actual));
        if (!success) { tok.error("duplicate template name {}", expected); }
    }
    return map;
}

auto compile_function(
    compiler& com,
    const token& tok,
    const std::string& full_name,
    const node_signature& node_sig,
    const node_stmt_ptr& body,
    const template_map& map = {}
)
    -> void
{
    const auto struct_name = com.current_struct.back().name;

    // member function - so check first argument is a pointer to an instance of the class
    if (struct_name != global_namespace) {
        tok.assert(node_sig.params.size() > 0, "member functions must have at least one arg");
        const auto actual = resolve_type(com, tok, node_sig.params[0].type);
        const auto expected = struct_name.add_const().add_ptr().add_const();
        
        const auto message = "first parameter to a struct member function must be a pointer to '{}', got '{}'";
        tok.assert(const_convertable_to(tok, actual, expected), message, struct_name, actual);
    }

    const auto id = com.functions.size();
    com.current_function.emplace_back(id, map);
    com.functions.emplace_back(full_name, id, variable_manager{true});
    const auto [it, success] = com.functions_by_name.emplace(full_name, id);
    tok.assert(success, "a function with the name '{}' already exists", full_name);
    
    variables(com).new_scope();

    auto sig = signature{};
    for (const auto& arg : node_sig.params) {
        const auto type = resolve_type(com, tok, arg.type);
        declare_var(com, tok, arg.name, type);
        sig.params.push_back(type);
    }
    sig.return_type = node_sig.return_type ? resolve_type(com, tok, node_sig.return_type) : null_type();
    current(com).sig = sig;

    // this can cause other template functions to be compiled so any references to function
    // info above may be invalidated!
    push_stmt(com, *body);

    if (!ends_in_return(*body)) {
        // Functions returning null don't need a final return, since we can just add it
        if (sig.return_type != null_type()) {
            tok.error("fn '{}' does not end in a return (needs {})", full_name, sig.return_type);
        }
        push_value(code(com), op::push_null, op::ret, std::uint64_t{1});
    }

    variables(com).pop_scope(code(com));
    com.current_function.pop_back();
}

// Temp: remove this for a more efficient function
auto string_replace(std::string subject, std::string_view search, std::string_view replace) -> std::string
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
    const auto type = push_expr(com, compile_type::val, node);
    if (type == null_type()) { push_value(code(com), op::print_null); }
    else if (type == bool_type()) { push_value(code(com), op::print_bool); }
    else if (type == char_type()) { push_value(code(com), op::print_char); }
    else if (type == i32_type()) { push_value(code(com), op::print_i32); }
    else if (type == i64_type()) { push_value(code(com), op::print_i64); }
    else if (type == u64_type()) { push_value(code(com), op::print_u64); }
    else if (type == f64_type()) { push_value(code(com), op::print_f64); }
    else if (type == char_type().add_span()) {
        push_value(code(com), op::print_char_span);
    }
    else if (type == nullptr_type()) { push_value(code(com), op::print_ptr); }
    else if (type.is_ptr()) { push_value(code(com), op::print_ptr); }
    else { tok.error("cannot print value of type {}", type); }
}

auto push_loop(compiler& com, std::function<void()> body) -> void
{
    variables(com).new_loop_scope();
    
    const auto begin_pos = code(com).size();
    variables(com).new_scope();
    body();
    variables(com).pop_scope(code(com));
    push_value(code(com), op::jump, begin_pos);

    // Fix up the breaks and continues
    const auto& control_flow = variables(com).get_loop_info();
    for (const auto idx : control_flow.breaks) {
        write_value(code(com), idx, code(com).size()); // Jump past end
    }
    for (const auto idx : control_flow.continues) {
        write_value(code(com), idx, begin_pos); // Jump to start
    }

    variables(com).pop_scope(code(com));
}


auto push_expr(compiler& com, compile_type ct, const node_literal_i32_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a i32 literal");
    push_value(code(com), op::push_i32, node.value);
    return i32_type();
}

auto push_expr(compiler& com, compile_type ct, const node_literal_i64_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a i64 literal");
    push_value(code(com), op::push_i64, node.value);
    return i64_type();
}

auto push_expr(compiler& com, compile_type ct, const node_literal_u64_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a u64 literal");
    push_value(code(com), op::push_u64, node.value);
    return u64_type();
}

auto push_expr(compiler& com, compile_type ct, const node_literal_f64_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a f64 literal");
    push_value(code(com), op::push_f64, node.value);
    return f64_type();
}

auto push_expr(compiler& com, compile_type ct, const node_literal_char_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a char literal");
    push_value(code(com), op::push_char, node.value);
    return char_type();
}

auto push_expr(compiler& com, compile_type ct, const node_literal_bool_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a bool literal");
    push_value(code(com), op::push_bool, node.value);
    return bool_type();
}

auto push_expr(compiler& com, compile_type ct, const node_literal_null_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a null literal");
    push_value(code(com), op::push_null);
    return null_type();
}

auto push_expr(compiler& com, compile_type ct, const node_literal_nullptr_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a nullptr literal");
    push_value(code(com), op::push_nullptr);
    return nullptr_type();
}

auto push_expr(compiler& com, compile_type ct, const node_literal_string_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a string literal");
    push_value(code(com), op::push_string_literal);
    push_value(code(com), insert_into_rom(com, node.value), node.value.size());
    return string_literal_type();
}

auto push_expr(compiler& com, compile_type ct, const node_unary_op_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a unary op");
    using tt = token_type;
    const auto type = push_expr(com, compile_type::val, *node.expr);
    node.token.assert(!type.is_type_value(), "invalid use of type expression");

    switch (node.token.type) {
        case tt::minus: {
            if (type == i32_type()) { push_value(code(com), op::i32_neg); return type; }
            if (type == i64_type()) { push_value(code(com), op::i64_neg); return type; }
            if (type == f64_type()) { push_value(code(com), op::f64_neg); return type; }
        } break;
        case tt::bang: {
            if (type == bool_type()) { push_value(code(com), op::bool_not); return type; }
        } break;
    }
    node.token.error("could not find op '{}{}'", node.token.type, type);
}

auto push_expr(compiler& com, compile_type ct, const node_binary_op_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a binary op");
    using tt = token_type;
    auto lhs = push_expr(com, compile_type::val, *node.lhs);
    auto rhs = push_expr(com, compile_type::val, *node.rhs);

    // TODO: Implement using == for comparing types
    node.token.assert(!lhs.is_type_value() && !rhs.is_type_value(), "invalid use of type expression");

    // Pointers can compare to nullptr
    if ((lhs.is_ptr() && rhs == nullptr_type()) || (rhs.is_ptr() && lhs == nullptr_type())) {
        switch (node.token.type) {
            case tt::equal_equal: { push_value(code(com), op::u64_eq); return bool_type(); }
            case tt::bang_equal:  { push_value(code(com), op::u64_ne); return bool_type(); }
        }
        node.token.error("could not find op '{} {} {}'", lhs, node.token.type, rhs);
    }

    if (lhs != rhs) node.token.error("could not find op '{} {} {}'", lhs, node.token.type, rhs);
    const auto& type = lhs;

    if (type.is_ptr()) {
        switch (node.token.type) {
            case tt::equal_equal: { push_value(code(com), op::u64_eq); return bool_type(); }
            case tt::bang_equal:  { push_value(code(com), op::u64_ne); return bool_type(); }
        }
    }
    else if (type == char_type()) {
        switch (node.token.type) {
            case tt::equal_equal: { push_value(code(com), op::char_eq); return bool_type(); }
            case tt::bang_equal:  { push_value(code(com), op::char_ne); return bool_type(); }
        }
    }
    else if (type == i32_type()) {
        switch (node.token.type) {
            case tt::plus:          { push_value(code(com), op::i32_add); return type;       }
            case tt::minus:         { push_value(code(com), op::i32_sub); return type;       }
            case tt::star:          { push_value(code(com), op::i32_mul); return type;       }
            case tt::slash:         { push_value(code(com), op::i32_div); return type;       }
            case tt::percent:       { push_value(code(com), op::i32_mod); return type;       }
            case tt::equal_equal:   { push_value(code(com), op::i32_eq); return bool_type(); }
            case tt::bang_equal:    { push_value(code(com), op::i32_ne); return bool_type(); }
            case tt::less:          { push_value(code(com), op::i32_lt); return bool_type(); }
            case tt::less_equal:    { push_value(code(com), op::i32_le); return bool_type(); }
            case tt::greater:       { push_value(code(com), op::i32_gt); return bool_type(); }
            case tt::greater_equal: { push_value(code(com), op::i32_ge); return bool_type(); }
        }
    }
    else if (type == i64_type()) {
        switch (node.token.type) {
            case tt::plus:          { push_value(code(com), op::i64_add); return type;       }
            case tt::minus:         { push_value(code(com), op::i64_sub); return type;       }
            case tt::star:          { push_value(code(com), op::i64_mul); return type;       }
            case tt::slash:         { push_value(code(com), op::i64_div); return type;       }
            case tt::percent:       { push_value(code(com), op::i64_mod); return type;       }
            case tt::equal_equal:   { push_value(code(com), op::i64_eq); return bool_type(); }
            case tt::bang_equal:    { push_value(code(com), op::i64_ne); return bool_type(); }
            case tt::less:          { push_value(code(com), op::i64_lt); return bool_type(); }
            case tt::less_equal:    { push_value(code(com), op::i64_le); return bool_type(); }
            case tt::greater:       { push_value(code(com), op::i64_gt); return bool_type(); }
            case tt::greater_equal: { push_value(code(com), op::i64_ge); return bool_type(); }
        }
    }
    else if (type == u64_type()) {
        switch (node.token.type) {
            case tt::plus:          { push_value(code(com), op::u64_add); return type;       }
            case tt::minus:         { push_value(code(com), op::u64_sub); return type;       }
            case tt::star:          { push_value(code(com), op::u64_mul); return type;       }
            case tt::slash:         { push_value(code(com), op::u64_div); return type;       }
            case tt::percent:       { push_value(code(com), op::u64_mod); return type;       }
            case tt::equal_equal:   { push_value(code(com), op::u64_eq); return bool_type(); }
            case tt::bang_equal:    { push_value(code(com), op::u64_ne); return bool_type(); }
            case tt::less:          { push_value(code(com), op::u64_lt); return bool_type(); }
            case tt::less_equal:    { push_value(code(com), op::u64_le); return bool_type(); }
            case tt::greater:       { push_value(code(com), op::u64_gt); return bool_type(); }
            case tt::greater_equal: { push_value(code(com), op::u64_ge); return bool_type(); }
        }
    }
    else if (type == f64_type()) {
        switch (node.token.type) {
            case tt::plus:          { push_value(code(com), op::f64_add); return type;       }
            case tt::minus:         { push_value(code(com), op::f64_sub); return type;       }
            case tt::star:          { push_value(code(com), op::f64_mul); return type;       }
            case tt::slash:         { push_value(code(com), op::f64_div); return type;       }
            case tt::equal_equal:   { push_value(code(com), op::f64_eq); return bool_type(); }
            case tt::bang_equal:    { push_value(code(com), op::f64_ne); return bool_type(); }
            case tt::less:          { push_value(code(com), op::f64_lt); return bool_type(); }
            case tt::less_equal:    { push_value(code(com), op::f64_le); return bool_type(); }
            case tt::greater:       { push_value(code(com), op::f64_gt); return bool_type(); }
            case tt::greater_equal: { push_value(code(com), op::f64_ge); return bool_type(); }
        }
    }
    else if (type == bool_type()) {
        switch (node.token.type) {
            case tt::ampersand_ampersand: { push_value(code(com), op::bool_and); return type; }
            case tt::bar_bar:             { push_value(code(com), op::bool_or);  return type; }
            case tt::equal_equal:         { push_value(code(com), op::bool_eq);  return type; }
            case tt::bang_equal:          { push_value(code(com), op::bool_ne);  return type; }
        }
    }

    node.token.error("could not find op '{} {} {}'", lhs, node.token.type, rhs);
}

auto push_expr(compiler& com, compile_type ct, const node_call_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a call expression");
    const auto type = type_of_expr(com, *node.expr);

    if (type.is_type_value()) { // constructor
        const auto obj_type = inner_type(type);
        if (node.args.empty()) { // default constructor
            push_value(code(com), op::push, com.types.size_of(obj_type));
            return obj_type;
        }
        const auto expected_params = get_constructor_params(com, obj_type);
        node.token.assert_eq(node.args.size(), expected_params.size(), 
                             "bad number of arguments to constructor call");
        for (std::size_t i = 0; i != node.args.size(); ++i) {
            push_copy_typechecked(com, *node.args.at(i), expected_params[i], node.token);
        }
        if (node.args.size() == 0) { // if the class has no data, it needs to be size 1
            push_value(code(com), op::push_null);
        }
        return obj_type;
    }
    else if (type.is_function_ptr()) { // function call
        const auto& info = std::get<type_function_ptr>(type);
        node.token.assert_eq(node.args.size(), info.param_types.size(), 
                             "invalid number of args for function call");

        auto args_size = std::size_t{0};
        for (std::size_t i = 0; i != node.args.size(); ++i) {
            push_copy_typechecked(com, *node.args.at(i), info.param_types[i], node.token);
            args_size += com.types.size_of(info.param_types[i]);
        }

        // push the function pointer and call it
        push_expr(com, compile_type::val, *node.expr);
        push_value(code(com), op::call, args_size);
        return *info.return_type;
    }
    else if (type.is_builtin()) { // builtin call
        const auto& info = std::get<type_builtin>(type);
        node.token.assert_eq(node.args.size(), info.args.size(), "bad number of arguments to builtin call");
        for (std::size_t i = 0; i != info.args.size(); ++i) {
            push_copy_typechecked(com, *node.args.at(i), info.args[i], node.token);
        }
        push_value(code(com), op::builtin_call, info.id);
        return *info.return_type;
    }
    else if (type.is_bound_method()) { // member function call
        const auto& info = std::get<type_bound_method>(type);
        node.token.assert_eq(node.args.size(), info.param_types.size() - 1, // instance ptr
                             "invalid number of args for function call");

        auto args_size = std::size_t{0};
    
        // cannot use push_copy_typechecked because the types mismatch, but the bound method
        // type just wraps a pointer to the instance, so this is fine
        push_expr(com, compile_type::val, *node.expr);
        args_size += com.types.size_of(info.param_types[0]);

        for (std::size_t i = 0; i != node.args.size(); ++i) {
            push_copy_typechecked(com, *node.args.at(i), info.param_types[i + 1], node.token);
            args_size += com.types.size_of(info.param_types[i]);
        }

        push_value(code(com), op::push_function_ptr, info.function_id, op::call, args_size);
        return *info.return_type;
    }
    else if (type.is_bound_builtin_method()) { // member builtin call
        const auto& info = std::get<type_bound_builtin_method>(type);
        if (info.type->is_array() && info.name == "size") {
            push_value(code(com), op::push_u64, array_length(*info.type));
            return u64_type();
        }
        else if (info.type->is_span() && info.name == "size") {
            push_expr(com, compile_type::val, *node.expr); // pointer to the span
            push_value(code(com), op::push_u64, sizeof(std::byte*), op::u64_add); // offset to the size value
            push_value(code(com), op::load, com.types.size_of(u64_type())); // load the size
            return u64_type();
        }
        else if (info.type->is_arena() && info.name == "size") {
            const auto type = push_expr(com, compile_type::val, *node.expr);
            auto_deref_pointer(com, type);
            push_value(code(com), op::load, com.types.size_of(u64_type())); // load the arena
            push_value(code(com), op::arena_size);
            return u64_type();
        }
    }

    node.token.error("unable to call non-callable type {}", type);
}

auto push_expr(compiler& com, compile_type ct, const node_array_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of an array expression");
    node.token.assert(!node.elements.empty(), "cannot have empty array literals");

    const auto inner_type = push_expr(com, compile_type::val, *node.elements.front());
    node.token.assert(!inner_type.is_type_value(), "invalid use of type expressions");
    for (const auto& element : node.elements | std::views::drop(1)) {
        const auto element_type = push_expr(com, compile_type::val, *element);
        node.token.assert_eq(element_type, inner_type, "array has mismatching element types");
    }
    return inner_type.add_array(node.elements.size());
}

auto push_expr(compiler& com, compile_type ct, const node_repeat_array_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a repeat array expression");
    node.token.assert(node.size != 0, "cannot have empty array literals");

    const auto inner_type = type_of_expr(com, *node.value);
    node.token.assert(!inner_type.is_type_value(), "invalid use of type expressions");
    for (std::size_t i = 0; i != node.size; ++i) {
        push_expr(com, compile_type::val, *node.value);
    }
    return inner_type.add_array(node.size);
}

auto push_expr(compiler& com, compile_type ct, const node_addrof_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of an addrof expression");
    const auto type = type_of_expr(com, *node.expr);
    if (type.is_type_value()) {
        return type_type{inner_type(type).add_ptr()};
    }
    if (com.types.size_of(type) == 0) {
        node.token.error("cannot take address of a type of size 0");
    }
    push_expr(com, compile_type::ptr, *node.expr);
    return type.add_ptr();
}

auto push_expr(compiler& com, compile_type ct, const node_sizeof_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a sizeof expression");
    const auto type = type_of_expr(com, *node.expr);
    if (type.is_type_value()) { // can call sizeof on a type directly
        push_value(code(com), op::push_u64, com.types.size_of(inner_type(type)));
    } else {
        push_value(code(com), op::push_u64, com.types.size_of(type));
    }
    return u64_type();
}

auto push_expr(compiler& com,compile_type ct, const node_span_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a span expression");
    if ((node.lower_bound && !node.upper_bound) || (!node.lower_bound && node.upper_bound)) {
        node.token.error("a span must either have both bounds set, or neither");
    }

    const auto type = type_of_expr(com, *node.expr);
    if (type.is_type_value()) {
        return type_type{inner_type(type).add_span()};
    }

    node.token.assert(
        type.is_array() || type.is_span(),
        "can only span arrays and other spans, not {}", type
    );

    push_expr(com, compile_type::ptr, *node.expr);

    // If we are a span, we want the address that it holds rather than its own address,
    // so switch the pointer by loading what it's pointing at.
    if (type.is_span()) {
        push_value(code(com), op::load, sizeof(std::byte*));
    }

    if (node.lower_bound) {// move first index of span up
        push_value(code(com), op::push_u64, com.types.size_of(inner_type(type)));
        const auto lower_bound_type = push_expr(com, compile_type::val, *node.lower_bound);
        node.token.assert_eq(lower_bound_type, u64_type(), "subspan indices must be u64");
        push_value(code(com), op::u64_mul);
        push_value(code(com), op::u64_add);
    }

    // next push the size to make up the second half of the span
    if (node.lower_bound && node.upper_bound) {
        push_expr(com, compile_type::val, *node.upper_bound);
        push_expr(com, compile_type::val, *node.lower_bound);
        push_value(code(com), op::u64_sub);
    } else if (type.is_span()) {
        // Push the span pointer, offset to the size, and load the size
        push_expr(com, compile_type::ptr, *node.expr);
        push_value(code(com), op::push_u64, sizeof(std::byte*), op::u64_add);
        push_value(code(com), op::load, com.types.size_of(u64_type()));
    } else {
        push_value(code(com), op::push_u64, array_length(type));
    }

    if (type.is_array()) {
        if (type.is_const) {
            return type.remove_array().add_const().add_span();
        }
        return type.remove_array().add_span();
    }
    else {  // is span
        if (type.is_const) {
            return type.remove_span().add_const().add_span();
        }
        return type;
    }
}

auto push_expr(compiler& com, compile_type ct, const node_typeof_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a typeof expression");
    return type_type{type_of_expr(com, *node.expr)};
}

auto push_expr(compiler& com, compile_type ct, const node_function_ptr_type_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a function ptr type expression");
    auto type = make_value<type_name>();
    auto& inner = type->emplace<type_function_ptr>();
    for (const auto& param : node.params) {
        inner.param_types.push_back(resolve_type(com, node.token, param));
    }
    inner.return_type = resolve_type(com, node.token, node.return_type);
    return type_type{type};
}

auto push_expr(compiler& com, compile_type ct, const node_const_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a const expression");
    const auto type = type_of_expr(com, *node.expr);
    node.token.assert(type.is_type_value(), "invalid use of a const-expr");
    return type_type{inner_type(type).add_const()};
}

auto push_expr(compiler& com, compile_type ct, const node_new_expr& node) -> type_name
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a new expression");
    const auto type = push_expr(com, compile_type::val, *node.expr); // first push new object to stack
    const auto type_size = com.types.size_of(type);
    if (node.count) { // we are allocating a span
        const auto count = push_expr(com, compile_type::val, *node.count);
        node.token.assert_eq(count, u64_type(), "wrong type for span size when allocating");
        const auto arena = push_expr(com, compile_type::val, *node.arena);
        const auto arena_stripped = auto_deref_pointer(com, arena); // can pass by value or pointer
        push_value(code(com), op::arena_alloc_array, type_size);
        return type.add_span();
    }
    else {
        const auto arena = push_expr(com, compile_type::val, *node.arena);
        const auto arena_stripped = auto_deref_pointer(com, arena); // can pass by value or pointer
        push_value(code(com), op::arena_alloc, type_size);
        return type.add_ptr();
    }
}

// TODO: Reorder the lookups, variables should probably be first
// A name can represent the following
//  - a variable name
//  - a function name
//  - a builtin function name
//  - a type name
void push_stmt(compiler& com, const node_function_stmt& stmt);
auto push_expr(compiler& com, compile_type ct, const node_name_expr& node) -> type_name
{
    // Firstly, check if it is a function that needs compiling (does val/ptr matter?)
    const auto full_name_no_templates = fn_name(com, node.token, global_namespace, node.name);
    const auto full_name = fn_name(com, node.token, global_namespace, node.name, node.templates);
    
    if (com.function_templates.contains(full_name_no_templates) && !get_function(com, full_name)) {
        const auto& ast = com.function_templates.at(full_name_no_templates);
        const auto map = build_template_map(com, node.token, ast.templates, node.templates);
        compile_function(com, node.token, full_name, ast.sig, ast.body, map);
    }

    // Next, it might be a template type
    const auto name = struct_name(com, node.token, node.name, node.templates);
    if (com.struct_templates.contains(node.name) && !com.types.contains(name)) {
        const auto& ast = com.struct_templates.at(node.name);

        auto map = build_template_map(com, node.token, ast.templates, node.templates);
        com.current_struct.emplace_back(name, map);
        auto fields = std::vector<type_field>{};
        for (const auto& p : ast.fields) {
            fields.emplace_back(type_field{ .name=p.name, .type=resolve_type(com, node.token, p.type) });
        }
        com.types.add(name, fields, map);

        for (const auto& func : ast.functions) {
            const auto& stmt = std::get<node_function_stmt>(*func);
            const auto func_name = fn_name(com, node.token, name, stmt.name);

            // Template functions only get compiled at the call site, so we just stash the ast
            const auto [it, success] = com.function_templates.emplace(func_name, stmt);
            node.token.assert(success, "function template named '{}' already defined", func_name);
        }

        com.current_struct.pop_back();
        return type_type{name};
    }

    // The name might be a function
    if (auto func = get_function(com, full_name)) {
        node.token.assert(ct == compile_type::val, "cannot take the address of a function ptr");
        push_value(code(com), op::push_u64, func->id);
        return type_function_ptr{ .param_types = func->sig.params, .return_type = func->sig.return_type };
    }

    // The name might be a builtin
    if (auto func = get_builtin(full_name)) {
        node.token.assert(ct == compile_type::val, "cannot take the address of a builtin");
        node.token.assert(node.templates.empty(), "builtins cannot be templated");
        return type_builtin{
            .name = func->name, .id = func->id, .args = func->args, .return_type = func->return_type
        };
    }

    // The name might be a type
    if (com.types.contains(name)) {
        node.token.assert(ct == compile_type::val, "cannot take the address of a type");
        return type_type{name};
    }

    // Otherwise, it must be a variable
    node.token.assert(node.templates.empty(), "variables cannot be templated ({})", node.name);
    if (ct == compile_type::ptr) {
        return push_var_addr(com, node.token, node.name);
    }
    const auto type = push_expr(com, compile_type::ptr, node);
    node.token.assert(!type.is_type_value(), "invalid use of type expressions");
    push_value(code(com), op::load, com.types.size_of(type));
    return type;
}

auto push_expr(compiler& com, compile_type ct, const node_field_expr& node) -> type_name
{
    const auto type = type_of_expr(com, *node.expr);
    node.token.assert(!type.is_type_value(), "fields of objects cannot currently by types");
    
    const auto stripped = strip_pointers(type);

    // Check if it is a function that needs compiling
    const auto full_name_no_templates = fn_name(com, node.token, stripped, node.field_name);
    const auto full_name = fn_name(com, node.token, stripped, node.field_name, node.templates);
    if (com.function_templates.contains(full_name_no_templates) && !get_function(com, full_name)) {
        const auto ast = com.function_templates.at(full_name_no_templates);
        com.current_struct.emplace_back(stripped, com.types.templates_of(stripped));
        const auto map = build_template_map(com, node.token, ast.templates, node.templates);
        compile_function(com, node.token, full_name, ast.sig, ast.body, map);
        com.current_struct.pop_back();
    }

    // Firstly, the field may be a member function
    if (auto info = get_function(com, full_name); info.has_value()) {
        node.token.assert(ct == compile_type::val, "cannot take the address of a bound method");
        push_expr(com, compile_type::ptr, *node.expr); // push pointer to the instance to bind to
        const auto stripped = auto_deref_pointer(com, type); // allow for field access through a pointer
        if (stripped.is_const && !info->sig.params[0].remove_ptr().is_const) {
            node.token.error("cannot bind a const variable to a non-const member function");
        }
        return type_bound_method{
            .param_types = info->sig.params,
            .return_type = info->sig.return_type,
            .function_name = info->name,
            .function_id = info->id
        };
    }

    // Next, it could be a member function on a builtin type
    if (type.is_array() || type.is_span() || type.is_arena()) {
        node.token.assert(node.templates.empty(), "builtin members functions cannot be templated");
        node.token.assert(ct == compile_type::val, "cannot take the address of a bound builtin method");
        if (node.field_name == "size") {
            push_expr(com, compile_type::ptr, *node.expr); // push pointer to the instance to bind to
            auto_deref_pointer(com, type); // allow for field access through a pointer
            return type_bound_builtin_method{ .name = "size", .type = type };
        }
    }

    // Otherwise, it's a data member
    node.token.assert(node.templates.empty(), "data members cannot be templated");
    push_expr(com, compile_type::ptr, *node.expr);
    auto_deref_pointer(com, type); // allow for field access through a pointer
    auto field_type = push_field_offset(com, node.token, stripped, node.field_name);
    push_value(code(com), op::u64_add); // modify ptr
    if (ct == compile_type::val) {
        push_value(code(com), op::load, com.types.size_of(field_type));
    }
    
    if (stripped.is_const) field_type.is_const = true; // propagate const to fields
    return field_type;
}

auto push_expr(compiler& com, compile_type ct, const node_deref_expr& node) -> type_name
{
    const auto type = push_expr(com, compile_type::val, *node.expr); // Push the address
    node.token.assert(type.is_ptr(), "cannot use deref operator on non-ptr type '{}'", type);
    if (ct == compile_type::val) {
        push_value(code(com), op::load, com.types.size_of(type));
    }
    return type.remove_ptr();
}

auto push_expr(compiler& com, compile_type ct, const node_subscript_expr& node) -> type_name
{
    const auto type = type_of_expr(com, *node.expr);
    if (type.is_type_value()) {
        if (auto index = std::get_if<node_literal_u64_expr>(&*node.index)) {
            return type_type{inner_type(type).add_array(index->value)};
        }
        node.token.error("index must be a u64 literal when delcaring an array type");
    }

    if (ct == compile_type::ptr) {
        const auto is_array = type.is_array();
        const auto is_span = type.is_span();
        node.token.assert(is_array || is_span, "subscript only supported for arrays and spans");

        push_expr(com, compile_type::ptr, *node.expr);

        // If we are a span, we want the address that it holds rather than its own address,
        // so switch the pointer by loading what it's pointing at.
        if (is_span) {
            push_value(code(com), op::load, sizeof(std::byte*));
        }

        // Offset pointer by (index * size)
        const auto inner = inner_type(type);
        const auto index = push_expr(com, compile_type::val, *node.index);
        node.token.assert_eq(index, u64_type(), "subscript argument must be u64, got {}", index);
        push_value(code(com), op::push_u64, com.types.size_of(inner));
        push_value(code(com), op::u64_mul);
        push_value(code(com), op::u64_add); // modify ptr
        if (is_array && type.is_const) {
            return inner.add_const(); // propagate const to elements
        }
        return inner;
    }

    const auto t = push_expr(com, compile_type::ptr, node);
    push_value(code(com), op::load, com.types.size_of(t));
    return t;
}


void push_stmt(compiler& com, const node_sequence_stmt& node)
{
    variables(com).new_scope();
    for (const auto& seq_node : node.sequence) {
        push_stmt(com, *seq_node);
    }
    variables(com).pop_scope(code(com));
}

void push_stmt(compiler& com, const node_loop_stmt& node)
{
    push_loop(com, [&] {
        push_stmt(com, *node.body);
    });
}

//loop {
//    if !<condition> break;
//    <body>
//}
void push_stmt(compiler& com, const node_while_stmt& node)
{
    push_loop(com, [&] {
        // if !<condition> break;
        const auto cond_type = push_expr(com, compile_type::val, *node.condition);
        node.token.assert_eq(cond_type, bool_type(), "while-stmt invalid condition");
        push_value(code(com), op::bool_not);
        push_value(code(com), op::jump_if_false);
        const auto jump_pos = push_value(code(com), std::uint64_t{0});
        push_break(com, node.token);
        write_value(code(com), jump_pos, code(com).size()); // Jump past the end if false      
        
        // <body>
        push_stmt(com, *node.body);
    });
}

//{
//    <<create temporary var if iter is an rvalue>>
//    idx = 0u;
//    size := <<length of iter>>;
//    loop {
//        if idx == size break;
//        name := iter[idx]~;
//        idx = idx + 1u;
//        <body>
//    }
//}
void push_stmt(compiler& com, const node_for_stmt& node)
{
    variables(com).new_scope();

    const auto iter_type = type_of_expr(com, *node.iter);

    const auto is_array = iter_type.is_array();
    const auto is_lvalue_span = iter_type.is_span() && is_lvalue_expr(*node.iter);
    node.token.assert(is_array || is_lvalue_span, "for-loops only supported for arrays and lvalue spans");

    // Need to create a temporary if we're using an rvalue
    if (is_rvalue_expr(*node.iter)) {
        push_expr(com, compile_type::val, *node.iter);
        declare_var(com, node.token, "#:iter", iter_type);
    }

    // idx := 0u;
    push_value(code(com), op::push_u64, std::uint64_t{0});
    declare_var(com, node.token, "#:idx", u64_type());

    // size := length of iter;
    if (iter_type.is_array()) {
        push_value(code(com), op::push_u64, array_length(iter_type));
        declare_var(com, node.token, "#:size", u64_type());
    } else {
        node.token.assert(is_lvalue_expr(*node.iter), "for-loops only supported for lvalue spans");
        push_expr(com, compile_type::ptr, *node.iter); // push pointer to span
        push_value(code(com), op::push_u64, sizeof(std::byte*));
        push_value(code(com), op::u64_add); // offset to the size value
        push_value(code(com), op::load, com.types.size_of(u64_type()));       
        declare_var(com, node.token, "#:size", u64_type());
    }

    push_loop(com, [&] {
        // if idx == size break;
        load_variable(com, node.token, "#:idx");
        load_variable(com, node.token, "#:size");
        push_value(code(com), op::u64_eq);
        push_value(code(com), op::jump_if_false);
        const auto jump_pos = push_value(code(com), std::uint64_t{0});
        push_break(com, node.token);
        write_value(code(com), jump_pos, code(com).size());

        // name := iter[idx]~;
        const auto iter_type = type_of_expr(com, *node.iter);
        const auto inner = inner_type(iter_type);
        if (is_rvalue_expr(*node.iter)) {
            push_var_addr(com, node.token, "#:iter");
        } else {
            push_expr(com, compile_type::ptr, *node.iter);
            if (iter_type.is_span()) {
                push_value(code(com), op::load, sizeof(std::byte*));
            }
        }
        load_variable(com, node.token, "#:idx");
        push_value(code(com), op::push_u64, com.types.size_of(inner));
        push_value(code(com), op::u64_mul);
        push_value(code(com), op::u64_add);
        declare_var(com, node.token, node.name, inner.add_ptr());

        // idx = idx + 1;
        load_variable(com, node.token, "#:idx");
        push_value(code(com), op::push_u64, std::uint64_t{1}, op::u64_add);
        save_variable(com, node.token, "#:idx");

        // main body
        push_stmt(com, *node.body);
    });

    variables(com).pop_scope(code(com));
}

void push_stmt(compiler& com, const node_if_stmt& node)
{
    const auto cond_type = push_expr(com, compile_type::val, *node.condition);
    node.token.assert_eq(cond_type, bool_type(), "if-stmt invalid condition");

    push_value(code(com), op::jump_if_false);
    const auto jump_pos = push_value(code(com), std::uint64_t{0});
    push_stmt(com, *node.body);

    if (node.else_body) {
        push_value(code(com), op::jump);
        const auto else_pos = push_value(code(com), std::uint64_t{0});
        const auto in_else_pos = code(com).size();
        push_stmt(com, *node.else_body);
        write_value(code(com), jump_pos, in_else_pos); // Jump into the else block if false
        write_value(code(com), else_pos, code(com).size()); // Jump past the end if false
    } else {
        write_value(code(com), jump_pos, code(com).size()); // Jump past the end if false
    }
}

void push_stmt(compiler& com, const node_struct_stmt& node)
{
    if (!node.templates.empty()) {
        const auto [it, success] = com.struct_templates.emplace(node.name, node);
        node.token.assert(success, "struct template named '{}' already defined", node.name);
        return;
    }

    const auto struct_name = make_type(com, node.name);
    const auto message = std::format("type '{}' already defined", node.name);
    node.token.assert(!com.types.contains(struct_name), "{}", message);
    node.token.assert(!com.functions_by_name.contains(node.name), "{}", message);

    com.current_struct.emplace_back(struct_name, template_map{});
    auto fields = std::vector<type_field>{};
    for (const auto& p : node.fields) {
        fields.emplace_back(p.name, resolve_type(com, node.token, p.type));
    }

    com.types.add(make_type(com, node.name), fields);
    for (const auto& function : node.functions) {
        push_stmt(com, *function);
    }
    com.current_struct.pop_back();
}

void push_stmt(compiler& com, const node_break_stmt& node)
{
    push_break(com, node.token);
}

void push_stmt(compiler& com, const node_continue_stmt& node)
{
    node.token.assert(variables(com).in_loop(), "cannot use 'continue' outside of a loop");
    variables(com).handle_loop_exit(code(com));
    push_value(code(com), op::jump);
    const auto pos = push_value(code(com), std::uint64_t{0}); // filled in later
    variables(com).get_loop_info().continues.push_back(pos);
}

auto push_stmt(compiler& com, const node_declaration_stmt& node) -> void
{
    auto type = node.explicit_type ? resolve_type(com, node.token, node.explicit_type)
                                   : type_of_expr(com, *node.expr);
    type.is_const = node.add_const;

    node.token.assert(!type.is_arena(), "cannot create copies of arenas");
    push_copy_typechecked(com, *node.expr, type, node.token);
    declare_var(com, node.token, node.name, type);
}

auto push_stmt(compiler& com, const node_arena_declaration_stmt& node) -> void
{
    const auto type = arena_type();
    push_value(code(com), op::arena_new);
    declare_var(com, node.token, node.name, type);
}

void push_stmt(compiler& com, const node_assignment_stmt& node)
{
    const auto lhs_type = type_of_expr(com, *node.position);
    node.token.assert(!lhs_type.is_const, "cannot assign to a const variable");
    push_copy_typechecked(com, *node.expr, lhs_type, node.token);
    const auto lhs = push_expr(com, compile_type::ptr, *node.position);
    push_value(code(com), op::save, com.types.size_of(lhs));
    return;
}

void push_stmt(compiler& com, const node_function_stmt& node)
{
    // We always ignore the template types here because it is either not a template function and so
    // this is in fact the full name, or it is and we use this as the key for the function_templates
    // map which is just the name without the template section.
    const auto struct_name = com.current_struct.back().name;
    const auto function_name = fn_name(com, node.token, struct_name, node.name);

    // Template functions only get compiled at the call site, so we just stash the ast
    if (!node.templates.empty()) {
        const auto [it, success] = com.function_templates.emplace(function_name, node);
        node.token.assert(success, "function template named '{}' already defined", function_name);
        return;
    }

    compile_function(com, node.token, function_name, node.sig, node.body);
}

void push_stmt(compiler& com, const node_expression_stmt& node)
{
    const auto type = push_expr(com, compile_type::val, *node.expr);
    push_value(code(com), op::pop, com.types.size_of(type));
}

void push_stmt(compiler& com, const node_return_stmt& node)
{
    node.token.assert(in_function(com), "can only return within functions");
    const auto return_type = push_expr(com, compile_type::val, *node.return_value);
    node.token.assert_eq(
        return_type, current(com).sig.return_type, "wrong return type"
    );
    variables(com).handle_function_exit(code(com));
    push_value(code(com), op::ret, com.types.size_of(return_type));
}

void push_stmt(compiler& com, const node_assert_stmt& node)
{
    const auto expr = push_expr(com, compile_type::val, *node.expr);
    node.token.assert_eq(expr, bool_type(), "bad assertion expression");
    const auto message = std::format("line {}", node.token.line);
    const auto index = insert_into_rom(com, message);
    push_value(code(com), op::assert, index, message.size());
}

void push_stmt(compiler& com, const node_print_stmt& node)
{
    if (node.message == "__dump_type") { // Hack to allow for an easy way to dump types of expressions
        std::print("__dump_type(\n");
        for (const auto& arg : node.args) {
            std::print("    {},\n", type_of_expr(com, *arg));
        }
        std::print(")\n");
        return;
    }

    const auto parts = string_split(string_replace(node.message, "\\n", "\n"), "{}");
    if (parts.size() != node.args.size() + 1) {
        node.token.error("Not enough args to fill all placeholders");
    }

    if (!parts.front().empty()) {
        push_value(code(com), op::push_string_literal);
        push_value(code(com), insert_into_rom(com, parts.front()), parts.front().size());
        push_value(code(com), op::print_char_span);
    }
    for (std::size_t i = 0; i != node.args.size(); ++i) {
        push_print_fundamental(com, *node.args.at(i), node.token);

        if (!parts[i+1].empty()) {
            push_value(code(com), op::push_string_literal);
            push_value(code(com), insert_into_rom(com, parts[i+1]), parts[i+1].size());
            push_value(code(com), op::print_char_span);
        }
    }
}

auto push_expr(compiler& com, compile_type ct, const node_expr& expr) -> type_name
{
    return std::visit([&](const auto& node) { return push_expr(com, ct, node); }, expr);
}

auto push_stmt(compiler& com, const node_stmt& root) -> void
{
    std::visit([&](const auto& node) { push_stmt(com, node); }, root);
}

}

auto compile(const anzu_module& ast) -> bytecode_program
{
    auto com = compiler{};
    com.functions.emplace_back("$main", 0, variable_manager{false});

    com.current_function.emplace_back(0, template_map{});
    com.current_struct.emplace_back(global_namespace, template_map{});
    variables(com).new_scope();
    push_stmt(com, *ast.root);
    variables(com).pop_scope(code(com));
    com.current_struct.pop_back();
    com.current_function.pop_back();

    push_value(com.functions[0].code, op::end_program);

    auto program = bytecode_program{};
    program.rom = com.rom;
    for (const auto& function : com.functions) {
        program.functions.push_back(bytecode_function{function.name, function.id, function.code});
    }
    return program;
}

}