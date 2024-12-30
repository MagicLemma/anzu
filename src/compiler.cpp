#include "compiler.hpp"

#include "lexer.hpp"
#include "object.hpp"
#include "parser.hpp"
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
#include <source_location>
#include <functional>

namespace anzu {
namespace {

struct expr_result
{
    type_name   type;
    const_value value = {};
};

// Returns the current function
auto current(compiler& com) -> function& {
    return com.functions[com.current_function.back()];
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

auto curr_module(const compiler& com) -> const std::filesystem::path&
{
    return com.current_module.back();
}

auto curr_struct(const compiler& com) -> const type_struct&
{
    return com.current_struct.back();
}

static const auto no_struct = type_struct{""};
enum class compile_type { val, ptr };

auto push_expr(compiler& com, compile_type ct, const node_expr& node) -> expr_result;
auto push_stmt(compiler& com, const node_stmt& root) -> void;
auto type_of_expr(compiler& com, const node_expr& node) -> expr_result;

auto get_fundamental_type(const std::string& name) -> std::optional<type_name>
{
    if (name == "null")   return type_name(type_null{});
    if (name == "bool")   return type_name(type_bool{});
    if (name == "char")   return type_name(type_char{});
    if (name == "i32")    return type_name(type_i32{});
    if (name == "i64")    return type_name(type_i64{});
    if (name == "u64")    return type_name(type_u64{});
    if (name == "f64")    return type_name(type_f64{});
    if (name == "module") return type_name(type_module{});
    if (name == "arena")  return type_name(type_arena{});
    if (name == "type")   return type_name(type_type{});
    return {};
}

auto get_type_value(const token& tok, const expr_result& result) -> type_name
{
    tok.assert(result.type.is<type_type>(), "expected type expression, got {}", result.type);
    tok.assert(result.value.is<type_name>(), "value of type needs to be known at compile time");
    return result.value.as<type_name>();
}

auto get_module_value(const token& tok, const expr_result& result) -> std::filesystem::path
{
    tok.assert(result.type.is<type_module>(), "expected module expression, got {}", result.type);
    tok.assert(result.value.is<std::filesystem::path>(), "value of module needs to be known at compile time");
    return result.value.as<std::filesystem::path>();
}

auto get_u64_value(const token& tok, const expr_result& result) -> std::uint64_t
{
    tok.assert(result.type.is<type_u64>(), "expected u64 type, got {}", result.type);
    tok.assert(result.value.is<std::uint64_t>(), "value of u64 needs to be known at compile time");
    return result.value.as<std::uint64_t>();
}

// If the given expression results in a type expression, return the inner type.
// Otherwise program is ill-formed.
auto resolve_type(compiler& com, const token& tok, const node_expr_ptr& expr) -> type_name
{
    const auto result = type_of_expr(com, *expr);
    
    // null is also their own types
    if (result.type.is<type_null>()) {
        return result.type;
    }

    return get_type_value(tok, result);
}

auto resolve_types(compiler& com, const token& tok, const std::vector<node_expr_ptr>& exprs)
{
    auto templates = std::vector<type_name>{};
    for (const auto& expr : exprs) { templates.push_back(resolve_type(com, tok, expr)); }
    return templates;
}

// Registers the given name in the current scope
void declare_var(
    compiler& com,
    const token& tok,
    const std::string& name,
    const type_name& type,
    const const_value& value = const_value{}
)
{
    if (!current(com).variables.declare(curr_module(com), name, type, com.types.size_of(type), value)) {
        tok.error("name already in use: '{}'", name);
    }
}

auto push_var_addr(compiler& com, const token& tok, const std::filesystem::path& module, const std::string& name) -> expr_result
{
    if (in_function(com)) {
        if (const auto var = variables(com).find(module, name); var.has_value()) {
            push_value(code(com), op::push_ptr_local, var->location);
            return {var->type};
        }
    }

    const auto var = globals(com).find(module, name);
    tok.assert(var.has_value(), "could not find variable '{}'\n", name);
    push_value(code(com), op::push_ptr_global, var->location);
    return {var->type};
}

auto push_var_val(compiler& com, const token& tok, const std::filesystem::path& module, const std::string& name) -> expr_result
{
    if (in_function(com)) {
        if (const auto var = variables(com).find(module, name); var.has_value()) {
            const auto size = com.types.size_of(var->type);
            if (size > 0) {
                push_value(code(com), op::push_val_local, var->location, size);
            }
            return { var->type, var->value };
        }
    }

    const auto var = globals(com).find(module, name);
    tok.assert(var.has_value(), "could not find variable '{}'\n", name);
    const auto size = com.types.size_of(var->type);
    if (size > 0) {
        push_value(code(com), op::push_val_global, var->location, size);
    }
    return { var->type, var->value };
}

// Given a type and a field name, push the offset of the fields position relative to its
// owner onto the stack
auto push_field_offset(
    compiler& com, const token& tok, const type_struct& type, const std::string& field_name
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
    
    tok.error("could not find field '{}' for type '{}'\n", field_name, type);
}

auto constructor_params(const compiler& com, const type_name& type) -> std::vector<type_name>
{
    if (type.is<type_struct>()) {
        auto params = std::vector<type_name>{};
        for (const auto& field : com.types.fields_of(type.as<type_struct>())) {
            params.emplace_back(field.type);
        }
        return params;
    }
    return {type};
}

// Gets the type of the expression by compiling it, then removes the added
// op codes to leave the program unchanged before returning the type.
auto type_of_expr(compiler& com, const node_expr& node) -> expr_result
{
    const auto program_size = code(com).size();
    const auto [type, value] = push_expr(com, compile_type::val, node);
    if (com.types.size_of(type) > 0) {
        code(com).resize(program_size);
    }
    return {type, value};
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
    while (t.is<type_ptr>()) {
        push_value(code(com), op::load, sizeof(std::byte*));
        t = t.remove_ptr();
    }
    return t;
}

auto strip_pointers(const type_name& type) -> type_name
{
    auto t = type;
    while (t.is<type_ptr>()) t = t.remove_ptr();
    return t;
}

auto const_convertable_to(const token& tok, const type_name& src, const type_name& dst) {
    if (src.is_const && !dst.is_const) {
        return false;
    }

    return std::visit(overloaded{
        [&](const type_array& l, const type_array& r) {
            return l.count == r.count && const_convertable_to(tok, *l.inner_type, *r.inner_type);
        },
        [&](const type_ptr& l, const type_ptr& r) {
            return const_convertable_to(tok, *l.inner_type, *r.inner_type);
        },
        [&](const type_span& l, const type_span& r) {
            return const_convertable_to(tok, *l.inner_type, *r.inner_type);
        },
        [&](const type_arena& l, const type_arena& r) { return true; },
        [&] <typename T> (const T& l, const T& r) { return l == r; },
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
    const auto actual = type_of_expr(com, expr).type.remove_const();
    const auto expected = expected_raw.remove_const();

    // Nothing to do for size 0 types
    if (actual == expected && com.types.size_of(actual) == 0) {
        return;
    }

    // null can convert to a null ptr, null span and an empty optional
    if (expected.is<type_ptr>() && actual.is<type_null>()) {
        push_value(code(com), op::push_u64, std::size_t{0}); // push a nullptr
        return;
    }
    if (expected.is<type_span>() && actual.is<type_null>()) {
        push_value(code(com), op::push_u64, std::size_t{0}); // push a nullptr
        push_value(code(com), op::push_u64, std::size_t{0}); // push the size
        return;
    }
    if (expected.is<type_optional>() && actual.is<type_null>()) {
        // push an empty object, the last byte is the bool which says its empty
        push_value(code(com), op::push, com.types.size_of(expected));
        return;
    }

    push_expr(com, compile_type::val, expr);

    // Let functions convert to function ptrs
    if (auto func = actual.get_if<type_function>(); func && func->to_pointer() == expected) {
        push_value(code(com), op::push_function_ptr, func->id); // push the id
        return;
    }

    // Let T convert to T?
    if (expected.is<type_optional>() && *expected.as<type_optional>().inner_type == actual) {
        push_value(code(com), op::push_bool, true); // the optional is engaged
        return;
    }

    if (actual.is<type_arena>() || expected.is<type_arena>()) {
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
    const std::vector<type_name>& types
)
    -> template_map
{
    tok.assert_eq(types.size(), names.size(), "bad number of template args");
    auto map = template_map{};
    for (const auto& [actual, expected] : std::views::zip(types, names)) {
        const auto [it, success] = map.emplace(expected, actual);
        if (!success) { tok.error("duplicate template name {}", expected); }
    }
    return map;
}

void match_placeholders(template_map& map, const token& tok, const type_name& actual, const type_name& expected)
{
    if (auto type = expected.get_if<type_placeholder>()) {
        const auto [it, success] = map.emplace(type->name, actual);
        tok.assert(success || it->second == actual,
                   "ambiguous template deduction, deduced {} as both {} and {}",
                   type->name, it->second, actual);
        return;
    }

    std::visit(overloaded{
        [&](const type_struct& a, const type_struct& e) {
            if (a.name == e.name && a.module == e.module && a.templates.size() == e.templates.size()) {
                for (const auto& [a_type, e_type] : std::views::zip(a.templates, e.templates)) {
                    match_placeholders(map, tok, a_type, e_type);
                }
            }
        },
        [&](const type_array& a, const type_array& e) {
            if (a.count == e.count) {
                match_placeholders(map, tok, *a.inner_type, *e.inner_type);
            }
        },
        [&](const type_ptr& a, const type_ptr& e) {
            match_placeholders(map, tok, *a.inner_type, *e.inner_type);
        },
        [&](const type_span& a, const type_span& e) {
            match_placeholders(map, tok, *a.inner_type, *e.inner_type);
        },
        [&](const type_function_ptr& a, const type_function_ptr& e) {
            if (a.param_types.size() == e.param_types.size()) {
                for (const auto& [a_type, e_type] : std::views::zip(a.param_types, e.param_types)) {
                    match_placeholders(map, tok, a_type, e_type);
                }
                match_placeholders(map, tok, *a.return_type, *e.return_type);
            }
        },
        [](const auto& a, const auto& e) {}
    }, actual, expected);
}

auto deduce_template_params(
    compiler& com,
    const token& tok,
    const std::vector<std::string>& names,
    const std::vector<node_expr_ptr>& sig_params,
    const std::vector<node_expr_ptr>& args
)
    -> std::vector<type_name>
{
    tok.assert_eq(args.size(), sig_params.size(), "invalid number of args to template function");

    auto placeholders = std::unordered_set<std::string>{};
    for (const auto name : names) placeholders.emplace(name);

    auto name_map = template_map{};
    for (const auto& [param, arg] : std::views::zip(sig_params, args)) {
        com.current_placeholders.push_back(&placeholders);
        const auto param_type = resolve_type(com, tok, param);
        com.current_placeholders.pop_back();
        const auto arg_type = type_of_expr(com, *arg).type;
        match_placeholders(name_map, tok, arg_type, param_type);
    }

    auto deduced_templates = std::vector<type_name>{};
    deduced_templates.reserve(names.size());
    for (const auto& name : names) {
        const auto it = name_map.find(name);
        tok.assert(it != name_map.end(), "unable to deduce type of template {}", name);
        deduced_templates.push_back(it->second);
    }
    return deduced_templates;
}

auto compile_function(
    compiler& com,
    const token& tok,
    const function_name& name,
    const std::vector<node_parameter>& ast_params,
    const node_expr_ptr& ast_return_type,
    const node_stmt_ptr& body,
    const template_map& map = {}
)
    -> void
{
    const auto id = com.functions.size();
    com.current_function.emplace_back(id);
    com.current_struct.emplace_back(name.struct_name);
    com.current_module.emplace_back(name.module);
    com.functions.emplace_back(name, id, variable_manager{true}, map);
    const auto [it, success] = com.functions_by_name.emplace(name, id);
    tok.assert(success, "a function with the name '{}' already exists", name);
    
    variables(com).new_scope();

    for (const auto& arg : ast_params) {
        const auto type = resolve_type(com, tok, arg.type);
        declare_var(com, tok, arg.name, type);
        current(com).params.push_back(type);
    }
    const auto return_type = ast_return_type ? resolve_type(com, tok, ast_return_type) : type_name{type_null{}};
    current(com).return_type = return_type;

    // this can cause other template functions to be compiled so any references to function
    // info above may be invalidated!
    push_stmt(com, *body);

    if (!ends_in_return(*body)) {
        // Functions returning null don't need a final return, since we can just add it
        if (!return_type.is<type_null>()) {
            tok.error("fn '{}' does not end in a return (needs {})", name, return_type);
        }
        push_value(code(com), op::push_null, op::ret, std::uint64_t{1});
    }

    variables(com).pop_scope(code(com));
    com.current_function.pop_back();
    com.current_struct.pop_back();
    com.current_module.pop_back();
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
    const auto type = push_expr(com, compile_type::val, node).type;
    std::visit(overloaded{
        [&] (type_null)          { push_value(code(com), op::print_null); },
        [&] (type_bool)          { push_value(code(com), op::print_bool); },
        [&] (type_char)          { push_value(code(com), op::print_char); },
        [&] (type_i32)           { push_value(code(com), op::print_i32);  },
        [&] (type_i64)           { push_value(code(com), op::print_i64);  },
        [&] (type_u64)           { push_value(code(com), op::print_u64);  },
        [&] (type_f64)           { push_value(code(com), op::print_f64);  },
        [&] (const type_ptr&)    { push_value(code(com), op::print_ptr);  },
        [&] (const type_span& t) {
            if (t.inner_type->is<type_char>()) {
                push_value(code(com), op::print_char_span);
            } else {
                tok.error("cannot print value of type {}", type);
            }
        },
        [&] (auto&&) {
            tok.error("cannot print value of type {}", type);
        }
    }, type);
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

auto load_module(compiler& com, const token& tok, const std::string& filepath) -> void
{
    // Add as an available module to the current module, and check for circular deps
    for (const auto& m : com.current_module | std::views::reverse) {
        if (m == filepath) {
            std::print("circular dependencey detected:\n");
            for (const auto& mod : com.current_module | std::views::reverse) {
                std::print("  - {}\n", mod.string());
                if (mod == m) tok.error("circular dependency");
            }
        }
    }

    // Already compiled, nothing more to do
    if (com.modules.contains(filepath)) {
        return; 
    }

    // Second, parse the module into its AST
    const auto path = std::filesystem::absolute(filepath);
    std::print("    - Parsing {}\n", filepath);
    const auto mod = parse(path);

    com.current_module.emplace_back(filepath);
    // We must unwrap the sequence statement like this since we do no want to introduce a new
    // scope while compiling this, otherwise all the variables will get popped after.
    tok.assert(std::holds_alternative<node_sequence_stmt>(*mod.root), "invalid module, top level must be a sequence");
    std::print("    - Compiling {}\n", filepath);
    for (const auto& node : std::get<node_sequence_stmt>(*mod.root).sequence) {
        push_stmt(com, *node);
    }
    com.current_module.pop_back();
    com.modules.emplace(filepath);
    std::print("    - Completed {}\n", filepath);
}

auto fetch_function(compiler& com, const token& tok, const function_name& name) -> type_function
{
    const auto key = name.as_template();

    // If the function doesn't exist, it may still be a template, if it is then compile it
    if (!com.functions_by_name.contains(name) && com.function_templates.contains(key)) {
        const auto& ast = com.function_templates.at(key);
        const auto map = build_template_map(com, tok, ast.templates, name.templates);
        compile_function(com, tok, name, ast.params, ast.return_type, ast.body, map);
    }

    tok.assert(com.functions_by_name.contains(name), "could not find function {}\n", name);
    const auto& fn = com.functions[com.functions_by_name.at(name)];
    return type_function{ .id = fn.id, .param_types=fn.params, .return_type=fn.return_type };
}

auto push_args_typechecked(compiler& com, const token& tok, const auto& args, const auto& expected_types) -> std::size_t
{
    tok.assert_eq(args.size(), expected_types.size(), "invalid number of args for function call");
    auto args_size = std::size_t{0};
    for (const auto& [arg, type] : std::views::zip(args, expected_types)) {
        push_copy_typechecked(com, *arg, type, tok);
        args_size += com.types.size_of(type);
    }
    return args_size;
}

auto compile_struct_template(
    compiler& com,
    const token& tok,
    const type_struct& name,
    const node_struct_stmt& stmt
)
    -> void
{
    const auto map = build_template_map(com, tok, stmt.templates, name.templates);
    com.current_struct.emplace_back(name);
    com.current_module.emplace_back(name.module);
    const auto success = com.types.add_type(name, map);
    tok.assert(success, "multiple definitions for struct {} found", name);
    for (const auto& p : stmt.fields) {
        const auto f = type_field{p.name, resolve_type(com, tok, p.type)};
        com.types.add_field(name, f);
    }
    com.current_struct.pop_back();
    com.current_module.pop_back();

    // Template functions only get compiled at the call site, so we just stash the ast
    // Otherwise, compile the functions now
    for (const auto& func : stmt.functions) {
        const auto& stmt = std::get<node_function_stmt>(*func);
        const auto fn_name = function_name{name.module, name, stmt.name};
        if (stmt.templates.empty()) {
            compile_function(com, tok, fn_name, stmt.params, stmt.return_type, stmt.body, map);
        } else {
            const auto fkey = type_function_template{name.module, name, stmt.name};
            const auto [it, success] = com.function_templates.emplace(fkey, stmt);
            tok.assert(success, "function template named '{}' already defined", fkey);
        }
    }
}

auto push_name_pack(
    compiler& com,
    const token& tok,
    const name_pack& np,
    const type_name& type,
    const const_value& expr_value = {}
) -> void
{
    std::visit(overloaded{
        [&](const std::string& name) {
            declare_var(com, tok, name, type, expr_value);
        },
        [&](const std::vector<name_pack>& names) {
            if (type.is<type_struct>()) {
                const auto fields = com.types.fields_of(type.as<type_struct>());
                tok.assert_eq(names.size(), fields.size(), "invalid number of args to unpack struct {} into", type);
                for (const auto& [name, field] : std::views::zip(names, fields)) {
                    auto field_type = field.type;
                    field_type.is_const = type.is_const;
                    push_name_pack(com, tok, name, field.type);
                }
            }
            else if (type.is<type_array>()) {
                const auto size = type.as<type_array>().count;
                tok.assert_eq(names.size(), size, "invalid number of args to unpack array {} into", type);
                auto field_type = *type.as<type_array>().inner_type;
                field_type.is_const = type.is_const;
                for (const auto& name : names) {
                    push_name_pack(com, tok, name, field_type);
                }
            }
            else {
                tok.error("objects of type {} cannot be unpacked", type);
            }
        }
    }, np.names);
}

auto push_expr(compiler& com, compile_type ct, const node_literal_i32_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a i32 literal");
    push_value(code(com), op::push_i32, node.value);
    return { type_i32{}, node.value };
}

auto push_expr(compiler& com, compile_type ct, const node_literal_i64_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a i64 literal");
    push_value(code(com), op::push_i64, node.value);
    return { type_i64{}, node.value };
}

auto push_expr(compiler& com, compile_type ct, const node_literal_u64_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a u64 literal");
    push_value(code(com), op::push_u64, node.value);
    return { type_u64{}, node.value };
}

auto push_expr(compiler& com, compile_type ct, const node_literal_f64_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a f64 literal");
    push_value(code(com), op::push_f64, node.value);
    return { type_f64{}, node.value };
}

auto push_expr(compiler& com, compile_type ct, const node_literal_char_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a char literal");
    push_value(code(com), op::push_char, node.value);
    return { type_char{}, node.value };
}

auto push_expr(compiler& com, compile_type ct, const node_literal_bool_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a bool literal");
    push_value(code(com), op::push_bool, node.value);
    return { type_bool{}, node.value };
}

auto push_expr(compiler& com, compile_type ct, const node_literal_null_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a null literal");
    push_value(code(com), op::push_null);
    return { type_null{} };
}

auto push_expr(compiler& com, compile_type ct, const node_literal_string_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a string literal");
    push_value(code(com), op::push_string_literal);
    auto str = string_replace(node.value, "\\n", "\n");
    str = string_replace(str, "\\r", "\r");
    str = string_replace(str, "\\t", "\t");
    push_value(code(com), insert_into_rom(com, str), str.size());
    return { string_literal_type() }; // TODO: Maybe support string literals at compile time?
}

// TODO: collapse compile-time values if possible
auto push_expr(compiler& com, compile_type ct, const node_unary_op_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a unary op");
    using tt = token_type;
    const auto type = push_expr(com, compile_type::val, *node.expr).type;

    switch (node.token.type) {
        case tt::minus: {
            if (type.is<type_i32>()) { push_value(code(com), op::i32_neg); return { type }; }
            if (type.is<type_i64>()) { push_value(code(com), op::i64_neg); return { type }; }
            if (type.is<type_f64>()) { push_value(code(com), op::f64_neg); return { type }; }
        } break;
        case tt::bang: {
            if (type.is<type_bool>()) { push_value(code(com), op::bool_not); return { type }; }
        } break;
    }
    node.token.error("[1] could not find op '{}{}'", node.token.type, type);
}

auto push_expr(compiler& com, compile_type ct, const node_binary_op_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a binary op");
    using tt = token_type;
    auto [lhs, lhs_value] = type_of_expr(com, *node.lhs);
    auto [rhs, rhs_value] = type_of_expr(com, *node.rhs);

    const auto push = [&] (anzu::op op_code) {
        push_expr(com, compile_type::val, *node.lhs);
        push_expr(com, compile_type::val, *node.rhs);
        push_value(code(com), op_code);
    };

    const auto push_ptr = [&] (const node_expr& expr) {
        if (type_of_expr(com, expr).type.is<type_null>()) {
            push_value(code(com), op::push_u64, std::size_t{0});
        } else {
            push_expr(com, compile_type::val, expr);
        }
    };

    // Allow for comparisons of types
    if (lhs.is<type_type>() && rhs.is<type_type>()) {
        const auto l = get_type_value(node.token, {lhs, lhs_value});
        const auto r = get_type_value(node.token, {rhs, rhs_value});
        switch (node.token.type) {
            case tt::equal_equal: {
                push_value(code(com), op::push_bool, l == r);
                return { type_bool{}, {l == r} };
            } break;
            case tt::bang_equal: {
                push_value(code(com), op::push_bool, l != r);
                return { type_bool{}, {l != r} };
            } break;
        }
        node.token.error("[2] could not find op '{} {} {}'", lhs, node.token.type, rhs);
    }

    // Types can compare to null, since null is also its own type, allows for T == null
    if ((lhs.is<type_type>() && rhs.is<type_null>()) || (rhs.is<type_type>() && lhs.is<type_null>())) {
        const auto lhs_inner = lhs_value.is<type_name>() ? lhs_value.as<type_name>() : type_name{type_null{}};
        const auto rhs_inner = rhs_value.is<type_name>() ? rhs_value.as<type_name>() : type_name{type_null{}};
        switch (node.token.type) {
            case tt::equal_equal: {
                push_value(code(com), op::push_bool, lhs_inner == rhs_inner);
                return { type_bool{}, {lhs_inner == rhs_inner} };
            } break;
            case tt::bang_equal: {
                push_value(code(com), op::push_bool, lhs_inner != rhs_inner);
                return { type_bool{}, {lhs_inner != rhs_inner} };
            } break;
        }
        node.token.error("[3] could not find op '{} {} {}'", lhs, node.token.type, rhs);
    }

    // Pointers can compare to null
    if ((lhs.is<type_ptr>() && rhs.is<type_null>()) || (rhs.is<type_ptr>() && lhs.is<type_null>())) {
        push_ptr(*node.lhs);
        push_ptr(*node.rhs); 
        switch (node.token.type) {
            case tt::equal_equal: { push_value(code(com), op::u64_eq); return { type_bool{} }; }
            case tt::bang_equal:  { push_value(code(com), op::u64_ne); return { type_bool{} }; }
        }
        node.token.error("[4] could not find op '{} {} {}'", lhs, node.token.type, rhs);
    }

    if (lhs != rhs) node.token.error("[5] could not find op '{} {} {}'", lhs, node.token.type, rhs);
    const auto& type = lhs;

    if (type.is<type_ptr>()) {
        switch (node.token.type) {
            case tt::equal_equal: { push(op::u64_eq); return { type_bool{} }; }
            case tt::bang_equal:  { push(op::u64_ne); return { type_bool{} }; }
        }
    }
    else if (type.is<type_char>()) {
        switch (node.token.type) {
            case tt::equal_equal: { push(op::char_eq); return { type_bool{} }; }
            case tt::bang_equal:  { push(op::char_ne); return { type_bool{} }; }
        }
    }
    else if (type.is<type_i32>()) {
        switch (node.token.type) {
            case tt::plus:          { push(op::i32_add); return { type };       }
            case tt::minus:         { push(op::i32_sub); return { type };       }
            case tt::star:          { push(op::i32_mul); return { type };       }
            case tt::slash:         { push(op::i32_div); return { type };       }
            case tt::percent:       { push(op::i32_mod); return { type };       }
            case tt::equal_equal:   { push(op::i32_eq); return { type_bool{} }; }
            case tt::bang_equal:    { push(op::i32_ne); return { type_bool{} }; }
            case tt::less:          { push(op::i32_lt); return { type_bool{} }; }
            case tt::less_equal:    { push(op::i32_le); return { type_bool{} }; }
            case tt::greater:       { push(op::i32_gt); return { type_bool{} }; }
            case tt::greater_equal: { push(op::i32_ge); return { type_bool{} }; }
        }
    }
    else if (type.is<type_i64>()) {
        switch (node.token.type) {
            case tt::plus:          { push(op::i64_add); return { type };       }
            case tt::minus:         { push(op::i64_sub); return { type };       }
            case tt::star:          { push(op::i64_mul); return { type };       }
            case tt::slash:         { push(op::i64_div); return { type };       }
            case tt::percent:       { push(op::i64_mod); return { type };       }
            case tt::equal_equal:   { push(op::i64_eq); return { type_bool{} }; }
            case tt::bang_equal:    { push(op::i64_ne); return { type_bool{} }; }
            case tt::less:          { push(op::i64_lt); return { type_bool{} }; }
            case tt::less_equal:    { push(op::i64_le); return { type_bool{} }; }
            case tt::greater:       { push(op::i64_gt); return { type_bool{} }; }
            case tt::greater_equal: { push(op::i64_ge); return { type_bool{} }; }
        }
    }
    else if (type.is<type_u64>()) {
        switch (node.token.type) {
            case tt::plus:          { push(op::u64_add); return { type };       }
            case tt::minus:         { push(op::u64_sub); return { type };       }
            case tt::star:          { push(op::u64_mul); return { type };       }
            case tt::slash:         { push(op::u64_div); return { type };       }
            case tt::percent:       { push(op::u64_mod); return { type };       }
            case tt::equal_equal:   { push(op::u64_eq); return { type_bool{} }; }
            case tt::bang_equal:    { push(op::u64_ne); return { type_bool{} }; }
            case tt::less:          { push(op::u64_lt); return { type_bool{} }; }
            case tt::less_equal:    { push(op::u64_le); return { type_bool{} }; }
            case tt::greater:       { push(op::u64_gt); return { type_bool{} }; }
            case tt::greater_equal: { push(op::u64_ge); return { type_bool{} }; }
        }
    }
    else if (type.is<type_f64>()) {
        switch (node.token.type) {
            case tt::plus:          { push(op::f64_add); return { type };       }
            case tt::minus:         { push(op::f64_sub); return { type };       }
            case tt::star:          { push(op::f64_mul); return { type };       }
            case tt::slash:         { push(op::f64_div); return { type };       }
            case tt::equal_equal:   { push(op::f64_eq); return { type_bool{} }; }
            case tt::bang_equal:    { push(op::f64_ne); return { type_bool{} }; }
            case tt::less:          { push(op::f64_lt); return { type_bool{} }; }
            case tt::less_equal:    { push(op::f64_le); return { type_bool{} }; }
            case tt::greater:       { push(op::f64_gt); return { type_bool{} }; }
            case tt::greater_equal: { push(op::f64_ge); return { type_bool{} }; }
        }
    }
    else if (type.is<type_bool>()) {
        switch (node.token.type) {
            case tt::ampersand_ampersand: {
                push_expr(com, compile_type::val, *node.lhs);
                push_value(code(com), op::jump_if_false);
                const auto jump_pos = push_value(code(com), std::size_t{0});
                push_expr(com, compile_type::val, *node.rhs);
                push_value(code(com), op::jump);
                const auto jump_pos2 = push_value(code(com), std::size_t{0});
                write_value(code(com), jump_pos, code(com).size());
                push_value(code(com), op::push_bool, false);
                write_value(code(com), jump_pos2, code(com).size());
                return { type };
            }
            case tt::bar_bar: {
                push_expr(com, compile_type::val, *node.lhs);
                push_value(code(com), op::jump_if_true);
                const auto jump_pos = push_value(code(com), std::size_t{0});
                push_expr(com, compile_type::val, *node.rhs);
                push_value(code(com), op::jump);
                const auto jump_pos2 = push_value(code(com), std::size_t{0});
                write_value(code(com), jump_pos, code(com).size());
                push_value(code(com), op::push_bool, true);
                write_value(code(com), jump_pos2, code(com).size());
                return { type };
            }
            case tt::equal_equal: { push(op::bool_eq); return { type }; }
            case tt::bang_equal:  { push(op::bool_ne); return { type }; }
        }
    }

    node.token.error("[6] could not find op '{} {} {}'", lhs, node.token.type, rhs);
}

auto push_expr(compiler& com, compile_type ct, const node_call_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a call expression");
    const auto [type, value] = type_of_expr(com, *node.expr);

    if (auto info = type.get_if<type_type>()) { // constructor
        const auto inner = get_type_value(node.token, {type, value});
        if (node.args.empty()) { // default constructor
            push_value(code(com), op::push, com.types.size_of(inner));
        } else {
            push_args_typechecked(com, node.token, node.args, constructor_params(com, inner));
        }
        return { inner };
    }
    else if (auto info = type.get_if<type_struct_template>()) {
        const auto& ast = com.struct_templates[*info];
        const auto params = ast.fields
                          | std::views::transform(&node_parameter::type)
                          | std::ranges::to<std::vector>();
        const auto templates = deduce_template_params(com, node.token, ast.templates, params, node.args);
        const auto name = type_struct{info->name, info->module, templates};
        if (!com.types.contains(name)) {
            compile_struct_template(com, node.token, name, ast);
        }
        push_args_typechecked(com, node.token, node.args, constructor_params(com, name));
        return { name };
    }
    else if (auto info = type.get_if<type_function_ptr>()) {
        const auto args_size = push_args_typechecked(com, node.token, node.args, info->param_types);
        push_expr(com, compile_type::val, *node.expr);
        push_value(code(com), op::call_ptr, args_size);
        return { *info->return_type };
    }
    else if (auto info = type.get_if<type_function>()) {
        const auto args_size = push_args_typechecked(com, node.token, node.args, info->param_types);
        push_value(code(com), op::call_static, info->id, args_size);
        return { *info->return_type };
    }
    else if (auto info = type.get_if<type_function_template>()) {
        const auto& ast = com.function_templates[*info];
        const auto params = ast.params
                          | std::views::transform(&node_parameter::type)
                          | std::ranges::to<std::vector>();
        const auto templates = deduce_template_params(com, node.token, ast.templates, params, node.args);
        const auto name = function_name{ info->module, info->struct_name, info->name, templates };
        const auto func = fetch_function(com, node.token, name);
        
        const auto args_size = push_args_typechecked(com, node.token, node.args, func.param_types);
        push_value(code(com), op::call_static, func.id, args_size);
        return { *func.return_type };
    }
    else if (auto info = type.get_if<type_bound_method>()) { // member function call
        // cannot use push_copy_typechecked because the types mismatch, but the bound method
        // type just wraps a pointer to the instance, so this is fine
        push_expr(com, compile_type::val, *node.expr);
        auto args_size = com.types.size_of(info->param_types[0]);
        args_size += push_args_typechecked(com, node.token, node.args, info->param_types | std::views::drop(1));
        push_value(code(com), op::call_static, info->id, args_size);
        return { *info->return_type };
    }
    else if (auto info = type.get_if<type_bound_method_template>()) { // member function call
        const auto& ast = com.function_templates[type_function_template{info->module, info->struct_name, info->name}];
      
        const auto sig_params = ast.params 
                              | std::views::drop(1) // can skip the self parameter
                              | std::views::transform(&node_parameter::type)
                              | std::ranges::to<std::vector>();

        const auto templates = deduce_template_params(com, node.token, ast.templates, sig_params, node.args);
        const auto name = function_name{info->module, info->struct_name, info->name, templates};
        const auto func = fetch_function(com, node.token, name);

        // cannot use push_copy_typechecked because the types mismatch, but the bound method
        // type just wraps a pointer to the instance, so this is fine
        push_expr(com, compile_type::val, *node.expr); // push pointer to the instance to bind to

        auto args_size = com.types.size_of(func.param_types[0]);
        args_size += push_args_typechecked(com, node.token, node.args, func.param_types | std::views::drop(1));
        push_value(code(com), op::call_static, func.id, args_size);
        return { *func.return_type };
    }

    node.token.error("unable to call non-callable type {}", type);
}

auto push_expr(compiler& com, compile_type ct, const node_template_expr& node) -> expr_result
{
    const auto templates = resolve_types(com, node.token, node.templates);
    const auto type = type_of_expr(com, *node.expr).type;

    if (auto info = type.get_if<type_function_template>()) {
        const auto name = function_name{ .module=info->module, .struct_name=info->struct_name, .name=info->name, .templates=templates };
        return { fetch_function(com, node.token, name) };
    }
    else if (auto info = type.get_if<type_bound_method_template>()) {
        push_expr(com, compile_type::val, *node.expr); // push pointer to the instance to bind to
        const auto name = function_name{info->module, info->struct_name, info->name, templates};
        return { fetch_function(com, node.token, name).to_bound_method() };
    }
    else if (auto info = type.get_if<type_struct_template>()) {
        const auto name = type_struct{info->name, info->module, templates};
        const auto key = type_struct_template{info->module, info->name};

        if (!com.types.contains(name) && com.struct_templates.contains(key)) {
            const auto& ast = com.struct_templates.at(key);
            compile_struct_template(com, node.token, name, ast);
        }

        node.token.assert(com.types.contains(name), "could not find struct {}", name);
        return { type_type{}, {type_name{name}} };
    }
    node.token.error("object of type {} can not be called with template parameters", type);
}

auto push_expr(compiler& com, compile_type ct, const node_array_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of an array expression");
    node.token.assert(!node.elements.empty(), "cannot have empty array literals");

    const auto inner_type = push_expr(com, compile_type::val, *node.elements.front()).type;
    node.token.assert(!inner_type.is<type_type>(), "invalid use of type expressions");
    for (const auto& element : node.elements | std::views::drop(1)) {
        const auto element_type = push_expr(com, compile_type::val, *element).type;
        node.token.assert_eq(element_type, inner_type, "array has mismatching element types");
    }
    return { inner_type.add_array(node.elements.size()) };
}

auto push_expr(compiler& com, compile_type ct, const node_repeat_array_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a repeat array expression");
    node.token.assert(node.size != 0, "cannot have empty array literals");

    const auto inner_type = type_of_expr(com, *node.value).type;
    node.token.assert(!inner_type.is<type_type>(), "invalid use of type expressions");
    for (std::size_t i = 0; i != node.size; ++i) {
        push_expr(com, compile_type::val, *node.value);
    }
    return { inner_type.add_array(node.size) };
}

auto push_expr(compiler& com, compile_type ct, const node_addrof_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of an addrof expression");
    if (!node.expr) {
        node.token.assert(
            curr_struct(com) != no_struct,
            "Tried to use a bare '&', but this only makes sense for member functions"
        );
        return { type_type{}, {type_name{curr_struct(com)}.add_ptr()} };
    }
    const auto result = type_of_expr(com, *node.expr);
    if (result.type.is<type_type>()) {
        const auto typeval = get_type_value(node.token, result);
        return { type_type{}, {typeval.add_ptr()} };
    }
    if (com.types.size_of(result.type) == 0) {
        node.token.error("cannot take address of a type of size 0 (type={})", result.type);
    }
    push_expr(com, compile_type::ptr, *node.expr);
    return { result.type.add_ptr() };
}

auto push_expr(compiler& com,compile_type ct, const node_span_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a span expression");
    if ((node.lower_bound && !node.upper_bound) || (!node.lower_bound && node.upper_bound)) {
        node.token.error("a span must either have both bounds set, or neither");
    }

    const auto [type, value] = type_of_expr(com, *node.expr);
    if (type.is<type_type>()) {
        const auto typeval = get_type_value(node.token, {type, value});
        return { type_type{}, {typeval.add_span()} };
    }

    node.token.assert(
        type.is<type_array>() || type.is<type_span>(),
        "can only span arrays and other spans, not {}", type
    );

    const auto type_size = [&] {
        if (type.is<type_array>()) return com.types.size_of(*type.as<type_array>().inner_type);
        return com.types.size_of(*type.as<type_span>().inner_type);
    }();

    push_expr(com, compile_type::ptr, *node.expr);

    // If we are a span, we want the address that it holds rather than its own address,
    // so switch the pointer by loading what it's pointing at.
    if (type.is<type_span>()) {
        push_value(code(com), op::load, sizeof(std::byte*));
    }

    // next push the size to make up the second half of the span
    if (node.lower_bound && node.upper_bound) {
        const auto lower_bound_type = push_expr(com, compile_type::val, *node.lower_bound).type;
        node.token.assert(lower_bound_type.is<type_u64>(), "subspan lower bound must be u64");
        const auto upper_bound_type = push_expr(com, compile_type::val, *node.upper_bound).type;
        node.token.assert(upper_bound_type.is<type_u64>(), "subspan upper bound must be u64");
        push_value(code(com), op::push_subspan, type_size);
    } else if (type.is<type_span>()) {
        // Push the span pointer, offset to the size, and load the size
        push_expr(com, compile_type::ptr, *node.expr);
        push_value(code(com), op::push_u64, sizeof(std::byte*), op::u64_add);
        push_value(code(com), op::load, com.types.size_of(type_u64{}));
    } else {
        push_value(code(com), op::push_u64, type.as<type_array>().count);
    }

    if (type.is<type_array>()) {
        if (type.is_const) {
            return { type.remove_array().add_const().add_span() };
        }
        return { type.remove_array().add_span() };
    }
    else {  // is span
        if (type.is_const) {
            return { type.remove_span().add_const().add_span() };
        }
        return { type };
    }
}

auto push_expr(compiler& com, compile_type ct, const node_function_ptr_type_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a function ptr type expression");
    auto type = type_name{};
    auto& inner = type.emplace<type_function_ptr>();
    for (const auto& param : node.params) {
        inner.param_types.push_back(resolve_type(com, node.token, param));
    }
    inner.return_type = resolve_type(com, node.token, node.return_type);
    return { type_type{}, {type} };
}

auto push_expr(compiler& com, compile_type ct, const node_const_expr& node) -> expr_result
{
    if (!node.expr) {
        node.token.assert(
            curr_struct(com) != no_struct,
            "Tried to use a bare 'const', but this only makes sense for member functions"
        );
        return { type_type{}, {type_name{curr_struct(com)}.add_const()} };
    }

    const auto [type, value] = type_of_expr(com, *node.expr);
    if (type.is<type_type>()) {
        node.token.assert(ct == compile_type::val, "cannot take the address of a const type-expression");
        const auto typeval = get_type_value(node.token, {type, value});
        return { type_type{}, {typeval.add_const()} };
    }

    const auto [t, v] = push_expr(com, ct, *node.expr);
    return { t.add_const(), v };
}

auto push_expr(compiler& com, compile_type ct, const node_optional_expr& node) -> expr_result
{
    const auto [type, value] = type_of_expr(com, *node.expr);
    node.token.assert(!type.is<type_null>(), "optional nulls not allowed, perhaps you want a bool?"); 
    
    // optional type declaration
    if (type.is<type_type>()) {
        const auto typeval = get_type_value(node.token, {type, value});
        return { type, {type_optional{typeval}} };
    }

    // otherwise, we are accessing the optional as the inner type (TODO: make safe)
    const auto ret = push_expr(com, ct, *node.expr);
    if (ct == compile_type::val) {
        push_value(code(com), op::pop, std::uint64_t{1}); // pop the bool if we're loading the val
    }
    return { ret.type.remove_optional() };
}

auto push_expr(compiler& com, compile_type ct, const node_new_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a new expression");
    const auto type = push_expr(com, compile_type::val, *node.expr).type; // first push new object to stack
    const auto type_size = com.types.size_of(type);
    if (node.original) { // we are reallocating a span
        const auto count = push_expr(com, compile_type::val, *node.count).type;
        node.token.assert(count.is<type_u64>(), "wrong type for span size when allocating");
        const auto arena = push_expr(com, compile_type::val, *node.arena).type;
        const auto arena_stripped = auto_deref_pointer(com, arena); // can pass by value or pointer
        const auto orig = push_expr(com, compile_type::val, *node.original).type;
        node.token.assert(orig.is<type_span>(), "original must be a span");
        node.token.assert_eq(orig.remove_span(), type, "original array and new array type mismatch");
        push_value(code(com), op::arena_realloc_array, type_size);
        return { type.add_span() };
    }
    else if (node.count) { // we are allocating a span
        const auto count = push_expr(com, compile_type::val, *node.count).type;
        node.token.assert(count.is<type_u64>(), "wrong type for span size when allocating");
        const auto arena = push_expr(com, compile_type::val, *node.arena).type;
        const auto arena_stripped = auto_deref_pointer(com, arena); // can pass by value or pointer
        push_value(code(com), op::arena_alloc_array, type_size);
        return { type.add_span() };
    }
    else {
        const auto arena = push_expr(com, compile_type::val, *node.arena).type;
        const auto arena_stripped = auto_deref_pointer(com, arena); // can pass by value or pointer
        push_value(code(com), op::arena_alloc, type_size);
        return { type.add_ptr() };
    }
}

// TODO: Reorder the lookups, variables should probably be first
// A name can represent the following
//  - a module
//  - a function template
//  - a struct template
//  - a struct
//  - a fundamental type
//  - a type alias for the current function template
//  - a type alias for the current struct template
//  - a function
//  - a placeholder
//  - a variable
void push_stmt(compiler& com, const node_function_stmt& stmt);
auto push_expr(compiler& com, compile_type ct, const node_name_expr& node) -> expr_result
{
    // It might be a function
    const auto fname = function_name{curr_module(com), no_struct, node.name};
    if (const auto it = com.functions_by_name.find(fname); it != com.functions_by_name.end()) {
        node.token.assert(ct == compile_type::val, "cannot take the address of a function");
        const auto& func = com.functions[it->second];
        return { type_function{func.id, func.params, func.return_type} };
    }

    // It might be a function template
    if (com.function_templates.contains(fname.as_template())) {
        node.token.assert(ct == compile_type::val, "cannot take the address of a function template");
        return { type_function_template{ .module = curr_module(com), .struct_name=no_struct, .name=node.name } };
    }

    // It might be a struct
    const auto sname = type_struct{node.name, curr_module(com)};
    if (com.types.contains(sname)) {
        return { type_type{}, {type_name{sname}} };
    }

    // It might be a struct template
    const auto stemp = type_struct_template{curr_module(com), node.name};
    if (com.struct_templates.contains(stemp)) {
        node.token.assert(ct == compile_type::val, "cannot take the address of a struct template");
        return { type_struct_template{ .module=curr_module(com), .name=node.name } };
    }

    // It might be a fundamental type
    if (const auto t = get_fundamental_type(node.name); t.has_value()) {
        node.token.assert(ct == compile_type::val, "cannot take the address of a type");
        return { type_type{}, {*t} };
    }

    // It might be a template placeholder for a type the needs to be deduced
    if (!com.current_placeholders.empty() && com.current_placeholders.back()->contains(node.name)) {
        return { type_type{}, {type_name{type_placeholder{node.name}}} };
    }

    // It might be one of the current functions template aliases
    const auto& map1 = current(com).templates;
    if (auto it = map1.find(node.name); it != map1.end()) {
        return { type_type{}, {it->second} };
    }

    // It might be one of the current structs template aliases
    const auto map2 = com.types.templates_of(curr_struct(com));
    if (auto it = map2.find(node.name); it != map2.end()) {
        return { type_type{}, {it->second} };
    }

    // Otherwise, it must be a variable
    if (ct == compile_type::ptr) {
        return push_var_addr(com, node.token, curr_module(com), node.name);
    }
    return push_var_val(com, node.token, curr_module(com), node.name);
}

auto push_expr(compiler& com, compile_type ct, const node_field_expr& node) -> expr_result
{
    const auto [type, value] = type_of_expr(com, *node.expr);

    // If the expression is a module, allow for accessing global variables, functions and structs
    if (auto info = type.get_if<type_module>()) {
        const auto filepath = get_module_value(node.token, {type, value});

        // It might be a function
        const auto fname = function_name{filepath, no_struct, node.name};
        if (const auto it = com.functions_by_name.find(fname); it != com.functions_by_name.end()) {
            node.token.assert(ct == compile_type::val, "cannot take the address of a function");
            const auto& func = com.functions[it->second];
            return { type_function{func.id, func.params, func.return_type } };
        }

        // It might be a function template
        if (com.function_templates.contains(fname.as_template())) {
            node.token.assert(ct == compile_type::val, "cannot take the address of a function template");
            return { type_function_template{ filepath, no_struct, node.name } };
        }

        // It might be a struct
        const auto sname = type_struct{ node.name, filepath };
        if (com.types.contains(sname)) {
            node.token.assert(ct == compile_type::val, "cannot take the address of a struct");
            return { type_type{}, {type_name{sname}} };
        }

        // It might be a struct template
        const auto skey = type_struct_template{filepath, node.name};
        if (com.struct_templates.contains(skey)) {
            node.token.assert(ct == compile_type::val, "cannot take the address of a struct template");
            return { type_struct_template{ filepath, node.name } };
        }

        // Otherwise, it must be a variable
        if (ct == compile_type::ptr) {
            return push_var_addr(com, node.token, filepath, node.name);
        }
        return push_var_val(com, node.token, filepath, node.name);
    }
    
    // If the expression is a type, allow for accessing the functions (only makes sense on structs)
    if (type.is<type_type>()) {
        const auto actual = get_type_value(node.token, {type, value});
        if (actual.is<type_struct>()) {
            const auto struct_info = actual.as<type_struct>();
            
            const auto fname = function_name{struct_info.module, struct_info, node.name};
            if (const auto it = com.functions_by_name.find(fname); it != com.functions_by_name.end()) {
                node.token.assert(ct == compile_type::val, "cannot take the address of a function");
                const auto& func = com.functions[it->second];
                return { type_function{func.id, func.params, func.return_type} };   
            }

            // It might be a function template
            if (com.function_templates.contains(fname.as_template())) {
                node.token.assert(ct == compile_type::val, "cannot take the address of a function template");
                return { type_function_template{fname.module, struct_info, node.name} };
            }

            node.token.error("can only access member functions from structs");
        }
    }
    
    const auto stripped = strip_pointers(type);
    node.token.assert(stripped.is<type_struct>(), "expected a struct type, got {}\n", stripped);
    const auto& struct_name = stripped.as<type_struct>();

    // It might be a member function
    const auto fname = function_name{struct_name.module, struct_name, node.name};
    if (const auto it = com.functions_by_name.find(fname); it != com.functions_by_name.end()) {
        const auto& info = com.functions[it->second];
        push_expr(com, compile_type::ptr, *node.expr); // push pointer to the instance to bind to
        auto_deref_pointer(com, type); // allow for field access through a pointer

        // check first argument is a pointer to an instance of the class
        node.token.assert(info.params.size() > 0, "member functions must have at least one arg");
        const auto actual = info.params[0];
        const auto expected = stripped.add_const().add_ptr().add_const();
        constexpr auto message = "tried to access static member function {} through an instance of {}, this can only be accessed directly on the class";
        node.token.assert(const_convertable_to(node.token, actual, expected), message, info.name, stripped);
        if (stripped.is_const && !actual.remove_ptr().is_const) {
            node.token.error("cannot bind a const variable to a non-const member function");
        }
        return { type_bound_method{info.id, info.params, info.return_type} };
    }

    // It might be a member function template
    if (com.function_templates.contains(fname.as_template())) {
        const auto& info = com.function_templates[fname.as_template()];
        push_expr(com, compile_type::ptr, *node.expr); // push pointer to the instance to bind to
        auto_deref_pointer(com, type); // allow for field access through a pointer
        
        // check first argument is a pointer to an instance of the class
        node.token.assert(info.params.size() > 0, "member functions must have at least one arg");
        com.current_module.emplace_back(fname.module);
        com.current_struct.emplace_back(fname.struct_name);
        const auto actual = resolve_type(com, node.token, info.params[0].type);
        com.current_struct.pop_back();
        com.current_module.pop_back();
        const auto expected = stripped.add_const().add_ptr().add_const();
        constexpr auto message = "tried to access static member function {} through an instance of {}, this can only be accessed directly on the class";
        node.token.assert(const_convertable_to(node.token, actual, expected), message, info.name, stripped);
        if (stripped.is_const && !actual.remove_ptr().is_const) {
            node.token.error("cannot bind a const variable to a non-const member function");
        }
        
        return { type_bound_method_template{ .module = struct_name.module, .struct_name=struct_name, .name=node.name } };
    }

    // Otherwise, it's a data member
    push_expr(com, compile_type::ptr, *node.expr);
    auto_deref_pointer(com, type); // allow for field access through a pointer
    auto field_type = push_field_offset(com, node.token, struct_name, node.name);
    push_value(code(com), op::u64_add); // modify ptr
    if (ct == compile_type::val) {
        push_value(code(com), op::load, com.types.size_of(field_type));
    }
    
    if (stripped.is_const) field_type.is_const = true; // propagate const to fields
    return { field_type };
}

auto push_expr(compiler& com, compile_type ct, const node_deref_expr& node) -> expr_result
{
    const auto type = push_expr(com, compile_type::val, *node.expr).type; // Push the address
    node.token.assert(type.is<type_ptr>(), "cannot use deref operator on non-ptr type '{}'", type);
    if (ct == compile_type::val) {
        push_value(code(com), op::load, com.types.size_of(type.remove_ptr()));
    }
    return { type.remove_ptr() };
}

auto push_expr(compiler& com, compile_type ct, const node_subscript_expr& node) -> expr_result
{
    const auto [type, value] = type_of_expr(com, *node.expr);
    if (type.is<type_type>()) {
        const auto size_result = type_of_expr(com, *node.index);
        const auto typeval = get_type_value(node.token, {type, value});
        const auto size = get_u64_value(node.token, size_result);
        return { type_type{}, {typeval.add_array(size)}};
    }

    const auto stripped = strip_pointers(type);
    const auto is_array = stripped.is<type_array>();
    const auto is_span = stripped.is<type_span>();
    node.token.assert(is_array || is_span, "subscript only supported for arrays and spans");

    push_expr(com, compile_type::ptr, *node.expr);
    auto_deref_pointer(com, type);

    // If we are a span, we want the address that it holds rather than its own address,
    // so switch the pointer by loading what it's pointing at.
    if (is_span) {
        push_value(code(com), op::load, sizeof(std::byte*));
    }

    // Offset pointer by (index * size)
    const auto inner = [&] {
        if (is_array) return *stripped.as<type_array>().inner_type;
        return *stripped.as<type_span>().inner_type;
    }();

    const auto index = push_expr(com, compile_type::val, *node.index).type;
    node.token.assert(index.is<type_u64>(), "subscript argument must be u64, got {}", index);
    const auto opcode = ct == compile_type::val ? op::nth_element_val : op::nth_element_ptr;
    push_value(code(com), opcode, com.types.size_of(inner));

    if (is_array && stripped.is_const) {
        return { inner.add_const() }; // propagate const to elements
    }
    return { inner };
}

auto push_expr(compiler& com, compile_type ct, const node_intrinsic_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of a @intrinsic function call");
    if (node.name == "len") {
        node.token.assert_eq(node.args.size(), 1, "@len only accepts one argument");
        const auto type = type_of_expr(com, *node.args[0]).type;
        if (auto info = type.get_if<type_array>()) {
            push_value(code(com), op::push_u64, info->count);
            return { type_u64{}, info->count };
        }
        else if (type.is<type_span>()) {
            push_expr(com, compile_type::ptr, *node.args[0]); // pointer to the span
            push_value(code(com), op::span_ptr_to_len);
            return { type_u64{} };
        }
        else if (type.is<type_arena>()) {
            const auto type = push_expr(com, compile_type::ptr, *node.args[0]).type;
            push_value(code(com), op::load, com.types.size_of(type_u64{})); // load the arena
            push_value(code(com), op::arena_size);
            return { type_u64{} };
        }
        else if (auto info = type.get_if<type_struct>()) {
            const auto name = function_name{.module=info->module, .struct_name=*info, .name="len"};
            const auto it = com.functions_by_name.find(name);
            node.token.assert(it != com.functions_by_name.end(), "cannot call @len on an object of type {}", type);

            const auto& func = com.functions[it->second];
            node.token.assert_eq(func.params.size(), 1, "@len must only take one argument");
            node.token.assert_eq(func.params[0], type.add_ptr(), "@len must only take a pointer to the object");
            node.token.assert_eq(func.return_type, type_name{type_u64{}}, "@len must return a u64");
            push_expr(com, compile_type::ptr, *node.args[0]);
            push_value(code(com), op::call_static, func.id, sizeof(std::byte*));
            return { type_u64{} };
        }
        else {
            node.token.error("cannot call 'len' on an object of type {}", type);
        }
        return { type_u64{} };
    }
    if (node.name == "size_of") {
        node.token.assert_eq(node.args.size(), 1, "@size_of only accepts one argument");
        const auto [type, value] = type_of_expr(com, *node.args[0]);
        if (type.is<type_type>()) { // can call sizeof on a type directly
            const auto typeval = get_type_value(node.token, {type, value});
            push_value(code(com), op::push_u64, com.types.size_of(typeval));
        } else {
            push_value(code(com), op::push_u64, com.types.size_of(type));
        }
        return { type_u64{} }; // TODO: can return as a compile-time value
    }
    if (node.name == "type_of") {
        node.token.assert_eq(node.args.size(), 1, "@type_of only accepts one argument");
        return { type_type{}, {type_of_expr(com, *node.args[0]).type} };
    }
    if (node.name == "type_name_of") {
        node.token.assert_eq(node.args.size(), 1, "@type_name_of only accepts one argument");
        const auto str = std::format("{}", type_of_expr(com, *node.args[0]).type);
        std::print("@type_name_of == {}\n", str);
        push_value(code(com), op::push_string_literal, insert_into_rom(com, str), str.size());
        return { string_literal_type() };
    }
    if (node.name == "copy") {
        node.token.assert_eq(node.args.size(), 2, "@copy requires two spans");
        const auto lhs = push_expr(com, ct, *node.args[0]).type;
        node.token.assert(lhs.is<type_span>(), "@copy bad first arg of type '{}'", lhs);
        node.token.assert(!lhs.as<type_span>().inner_type->is_const, "@copy cannot write through a const span");
        const auto rhs = push_expr(com, ct, *node.args[1]).type;
        node.token.assert(rhs.is<type_span>(), "@copy bad second arg of type '{}'", rhs);
        node.token.assert_eq(lhs, rhs, "@copy args must be of the same span type");
        push_value(code(com), op::memcpy, com.types.size_of(*lhs.as<type_span>().inner_type));
        return { type_null{} };
    }
    if (node.name == "compare") {
        node.token.assert_eq(node.args.size(), 2, "@compare requires two arguments");
        const auto lhs = push_expr(com, ct, *node.args[0]).type;
        node.token.assert(lhs.is<type_ptr>(), "@compare bad first arg of type '{}'", lhs);
        const auto rhs = push_expr(com, ct, *node.args[1]).type;
        node.token.assert(rhs.is<type_ptr>(), "@compare bad second arg of type '{}'", rhs);
        node.token.assert_eq(rhs.remove_ptr().remove_const(),
                             lhs.remove_ptr().remove_const(),
                             "@copy args must be of the same type");
        push_value(code(com), op::memcmp, com.types.size_of(lhs.remove_ptr()));
        return { type_bool{} };
    }
    if (node.name == "import") {
        node.token.assert(com.current_function.size() == 1, "can only import modules at the top level");
        node.token.assert_eq(node.args.size(), 1, "@module only accepts one argument");
        node.token.assert(std::holds_alternative<node_literal_string_expr>(*node.args[0]), "@module requires a string literal");
        const auto filepath = std::get<node_literal_string_expr>(*node.args[0]).value;
        load_module(com, node.token, filepath);
        return { type_module{}, filepath };
    }
    if (node.name == "fn_ptr") {
        node.token.assert_eq(node.args.size(), 1, "@fn_ptr only accepts one argument");
        const auto type = type_of_expr(com, *node.args[0]).type;
        node.token.assert(type.is<type_function>(), "can only convert functions to function pointers");
        const auto& info = type.as<type_function>();
        push_value(code(com), op::push_function_ptr, info.id);
        return { type_function_ptr{.param_types=info.param_types, .return_type=info.return_type} };
    }
    if (node.name == "is_fundamental") {
        node.token.assert_eq(node.args.size(), 1, "@is_fundamental only accepts one argument");
        const auto result = type_of_expr(com, *node.args[0]);
        const auto type = get_type_value(node.token, result);

        const auto is_fundamental = std::visit(overloaded{
            [&](type_null)   { return true;  },
            [&](type_bool)   { return true;  },
            [&](type_char)   { return true;  },
            [&](type_i32)    { return true;  },
            [&](type_i64)    { return true;  },
            [&](type_u64)    { return true;  },
            [&](type_f64)    { return true;  },
            [&](type_arena)  { return true;  },
            [&](type_module) { return true;  },
            [&](type_type)   { return true;  },
            [&](auto&&)      { return false; }
        }, type);

        push_value(code(com), op::push_bool, is_fundamental);
        return { type_bool{}, {is_fundamental} };
    }
    if (node.name == "is_span") {
        node.token.assert_eq(node.args.size(), 1, "@is_span only accepts one argument");
        const auto result = type_of_expr(com, *node.args[0]);
        const auto type = get_type_value(node.token, result);

        const auto is_span = type.is<type_span>();
        push_value(code(com), op::push_bool, is_span);
        return { type_bool{}, {is_span} };
    }
    if (node.name == "read_file") {
        const auto char_span = type_name{type_char{}}.add_const().add_span();
        const auto arena_ptr = type_name{type_arena{}}.add_ptr();

        node.token.assert_eq(node.args.size(), 2, "@read_file requires a filename and arena");
        const auto file_type = push_expr(com, compile_type::val, *node.args[0]).type;
        node.token.assert_eq(file_type, char_span, "incorrect type for file path");
        const auto arena_type = push_expr(com, compile_type::val, *node.args[1]).type;
        node.token.assert_eq(arena_type, arena_ptr, "incorrect type for arena");
        push_value(code(com), op::load, sizeof(std::byte*)); // load the arena
        push_value(code(com), op::read_file);
        return { char_span };
    }

    // For optionals, returns the bool that says if the optional has a value or not.
    if (node.name == "has_value") {
        node.token.assert_eq(node.args.size(), 1, "@has_value only accepts one argument");
        const auto type = push_expr(com, compile_type::ptr, *node.args[0]).type;
        node.token.assert(type.is<type_optional>(), "@has_value only operates on optionals");
        const auto inner = *type.as<type_optional>().inner_type;
        push_value(code(com), op::push_u64, com.types.size_of(inner), op::u64_add);
        push_value(code(com), op::load, std::uint64_t{1}); // load the bool
        return { type_bool{} };
    }

    node.token.error("no intrisic function named @{} exists", node.name);
}

auto push_expr(compiler& com, compile_type ct, const node_as_expr& node) -> expr_result
{
    node.token.assert(ct == compile_type::val, "cannot take the address of an 'as' statement");
    const auto src_type = push_expr(com, ct, *node.expr).type;
    const auto result = push_expr(com, ct, *node.type);
    const auto dst_type = get_type_value(node.token, result);

    std::visit(overloaded{
        [&] <class T> (const T&, const T&) {}, // noop

        [&](type_null, type_i64) { push_value(code(com), op::null_to_i64); },
        [&](type_bool, type_i64) { push_value(code(com), op::bool_to_i64); },
        [&](type_char, type_i64) { push_value(code(com), op::char_to_i64); },
        [&](type_i32,  type_i64) { push_value(code(com), op::i32_to_i64);  },
        [&](type_u64,  type_i64) { push_value(code(com), op::u64_to_i64);  },
        [&](type_f64,  type_i64) { push_value(code(com), op::f64_to_i64);  },

        [&](type_null, type_u64) { push_value(code(com), op::null_to_u64); },
        [&](type_bool, type_u64) { push_value(code(com), op::bool_to_u64); },
        [&](type_char, type_u64) { push_value(code(com), op::char_to_u64); },
        [&](type_i32,  type_u64) { push_value(code(com), op::i32_to_u64);  },
        [&](type_i64,  type_u64) { push_value(code(com), op::i64_to_u64);  },
        [&](type_f64,  type_u64) { push_value(code(com), op::f64_to_u64);  },

        [&](const auto&, const auto&) {
            node.token.error("cannot convert expression of type '{}' to '{}'", src_type, dst_type);
        }
    }, src_type, dst_type);

    return { dst_type };
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
        const auto cond_type = push_expr(com, compile_type::val, *node.condition).type;
        node.token.assert_eq(cond_type, type_name{type_bool{}}, "while-stmt invalid condition");
        push_value(code(com), op::jump_if_true);
        const auto jump_pos = push_value(code(com), std::uint64_t{0});
        push_break(com, node.token);
        write_value(code(com), jump_pos, code(com).size()); // Jump past the end if false      
        
        // <body>
        push_stmt(com, *node.body);
    });
}

//{
//    var obj := <container>;
//    var idx = 0u;
//    var size := <<length of iter>>;
//    loop {
//        if idx == size break;
//        var name := obj[idx] or obj[idx]&;
//        idx = idx + 1u;
//        <body>
//    }
//}
void push_for_loop_span(compiler& com, const node_for_stmt& node, const type_span& iter_type)
{
    const auto inner = *iter_type.inner_type;

    // Declare the iterable, but here can we destructure the span into a pointer + size
    declare_var(com, node.token, "$iter", inner.add_ptr());
    declare_var(com, node.token, "$size", type_u64{});

    // var idx := 0u;
    push_value(code(com), op::push_u64, std::uint64_t{0});
    declare_var(com, node.token, "$idx", type_u64{});

    push_loop(com, [&] {
        // if idx == size break;
        push_var_val(com, node.token, curr_module(com), "$idx");
        push_var_val(com, node.token, curr_module(com), "$size");
        push_value(code(com), op::u64_eq, op::jump_if_false);
        const auto jump_pos = push_value(code(com), std::uint64_t{0});
        push_break(com, node.token);
        write_value(code(com), jump_pos, code(com).size());

        // var name := iter[idx] or iter[idx]&;
        push_var_val(com, node.token, curr_module(com), "$iter");
        push_var_val(com, node.token, curr_module(com), "$idx");
        if (node.is_ptr) {
            node.token.assert(std::holds_alternative<std::string>(node.names.names), "span-based for loop cannot take a pointer to an unpacking");
            push_value(code(com), op::nth_element_ptr, com.types.size_of(inner));
            push_name_pack(com, node.token, node.names, inner.add_ptr());
        } else {
            push_value(code(com), op::nth_element_val, com.types.size_of(inner));
            push_name_pack(com, node.token, node.names, inner);
        }

        // idx = idx + 1;
        push_var_val(com, node.token, curr_module(com), "$idx");
        push_value(code(com), op::push_u64, std::uint64_t{1}, op::u64_add);
        push_var_addr(com, node.token, curr_module(com), "$idx");
        push_value(code(com), op::save, com.types.size_of(type_u64{}));

        // main body
        push_stmt(com, *node.body);
    });
}

//{
//    var obj := <container>;
//    loop {
//        if !obj.valid() { break; }
//        var name := obj.next();
//        <body>
//    }
//}
void push_for_loop_iterator(compiler& com, const node_for_stmt& node, const type_struct& type)
{
    declare_var(com, node.token, "$iter", type);

    node.token.assert(!node.is_ptr, "an iterator-based loop cannot have a pointer argument");

    // Fetch and verify the valid() function
    const auto valid_name = function_name{.module=type.module, .struct_name=type, .name="valid"};
    const auto valid_it = com.functions_by_name.find(valid_name);
    node.token.assert(valid_it != com.functions_by_name.end(), "iterable type requires 'valid' function");
    const auto valid_fn = com.functions[valid_it->second];
    node.token.assert_eq(valid_fn.params.size(), 1, "'valid' must only take one arg");
    node.token.assert_eq(valid_fn.params[0], type_name{type}.add_const().add_ptr(), "'valid' first arg must be a self pointer to const");
    node.token.assert_eq(valid_fn.return_type, type_name{type_bool{}}, "'valid' must return bool");

    // Fetch and verify the next() function
    const auto next_name = function_name{.module=type.module, .struct_name=type, .name="next"};
    const auto next_it = com.functions_by_name.find(next_name);
    node.token.assert(next_it != com.functions_by_name.end(), "iterable type requires 'next' function");
    const auto next_fn = com.functions[next_it->second];
    node.token.assert_eq(next_fn.params.size(), 1, "'next' must only take one arg");
    node.token.assert_eq(next_fn.params[0], type_name{type}.add_ptr(), "'next' first arg must be a self pointer");

    push_loop(com, [&] {
        // if !obj.valid() { break; }
        push_var_addr(com, node.token, curr_module(com), "$iter");
        push_value(code(com), op::call_static, valid_fn.id, sizeof(std::byte*));
        push_value(code(com), op::bool_not, op::jump_if_false);
        const auto jump_pos = push_value(code(com), std::uint64_t{0});
        push_break(com, node.token);
        write_value(code(com), jump_pos, code(com).size());

        // var name := obj.next();
        push_var_addr(com, node.token, curr_module(com), "$iter");
        push_value(code(com), op::call_static, next_fn.id, sizeof(std::byte*));
        push_name_pack(com, node.token, node.names, next_fn.return_type);

        // main body
        push_stmt(com, *node.body);
    });
}

void push_stmt(compiler& com, const node_for_stmt& node)
{
    variables(com).new_scope();

    // Declare the span ptr and size as two separate variables
    const auto iter_type = push_expr(com, compile_type::val, *node.iter).type;

    if (iter_type.is<type_span>()) {
        push_for_loop_span(com, node, iter_type.as<type_span>());
    }
    else if (iter_type.is<type_struct>()) {
        push_for_loop_iterator(com, node, iter_type.as<type_struct>());
    }
    else {
        node.token.error("cannot iterator over object of type '{}'", iter_type);
    }
    
    variables(com).pop_scope(code(com));
}

void push_stmt(compiler& com, const node_if_stmt& node)
{
    const auto program_size = code(com).size();

    const auto [cond_type, cond_value] = push_expr(com, compile_type::val, *node.condition);
    
    node.token.assert_eq(cond_type, type_name{type_bool{}}, "if-stmt invalid condition");
    if (cond_value.is<bool>()) {
        code(com).resize(program_size); // Remove the code to push the value at runtime
        if (cond_value.as<bool>()) {
            push_stmt(com, *node.body);
        } else if (node.else_body) {
            push_stmt(com, *node.else_body);
        }
        return;
    }
    node.token.assert(!cond_value.has_value(), "compiler error: condition has a non-bool value when it shouldn't");
    
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
        const auto key = type_struct_template{curr_module(com), node.name};
        const auto [it, success] = com.struct_templates.emplace(key, node);
        node.token.assert(success, "struct template named '<{}>.{}' already defined", curr_module(com).string(), node.name);
        return;
    }

    const auto sname = type_struct{ .name=node.name, .module=curr_module(com) };
    com.current_struct.emplace_back(sname);
    const auto success = com.types.add_type(sname);
    node.token.assert(success, "multiple definitions for struct {} found", sname);
    for (const auto& p : node.fields) {
        const auto f = type_field{p.name, resolve_type(com, node.token, p.type)};
        com.types.add_field(sname, f);
    }

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
    const auto [expr_type, expr_value] = type_of_expr(com, *node.expr);
    auto type = node.explicit_type ? resolve_type(com, node.token, node.explicit_type)
                                   : expr_type;
    type.is_const = node.add_const;
    node.token.assert(!type.is<type_arena>(), "cannot create copies of arenas");
    push_copy_typechecked(com, *node.expr, type, node.token);
    push_name_pack(com, node.token, node.names, type, expr_value);
}

auto push_stmt(compiler& com, const node_arena_declaration_stmt& node) -> void
{
    push_value(code(com), op::arena_new);
    declare_var(com, node.token, node.name, type_arena{});
}

void push_stmt(compiler& com, const node_assignment_stmt& node)
{
    const auto lhs_type = type_of_expr(com, *node.position).type;
    node.token.assert(!lhs_type.is_const, "cannot assign to a const variable");
    push_copy_typechecked(com, *node.expr, lhs_type, node.token);
    const auto lhs = push_expr(com, compile_type::ptr, *node.position).type;
    push_value(code(com), op::save, com.types.size_of(lhs));
    return;
}

void push_stmt(compiler& com, const node_function_stmt& node)
{
    // Template functions only get compiled at the call site, so we just stash the ast
    if (!node.templates.empty()) {
        const auto key = type_function_template{curr_module(com), curr_struct(com), node.name};
        const auto [it, success] = com.function_templates.emplace(key, node);
        node.token.assert(success, "function template named '{}' already defined", key);
    } else {
        const auto name = function_name{curr_module(com), curr_struct(com), node.name};
        compile_function(com, node.token, name, node.params, node.return_type, node.body);
    }
}

void push_stmt(compiler& com, const node_expression_stmt& node)
{
    const auto type = push_expr(com, compile_type::val, *node.expr).type;
    push_value(code(com), op::pop, com.types.size_of(type));
}

void push_stmt(compiler& com, const node_return_stmt& node)
{
    node.token.assert(in_function(com), "can only return within functions");
    const auto return_type = current(com).return_type;
    push_copy_typechecked(com, *node.return_value, return_type, node.token);
    variables(com).handle_function_exit(code(com));
    push_value(code(com), op::ret, com.types.size_of(return_type));
}

void push_stmt(compiler& com, const node_assert_stmt& node)
{
    const auto expr = push_expr(com, compile_type::val, *node.expr).type;
    node.token.assert_eq(expr, type_name{type_bool{}}, "bad assertion expression");
    const auto message = std::format("line {}", node.token.line);
    const auto index = insert_into_rom(com, message);
    push_value(code(com), op::assert, index, message.size());
}

void push_stmt(compiler& com, const node_print_stmt& node)
{
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

auto push_expr(compiler& com, compile_type ct, const node_expr& expr) -> expr_result
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
    const auto fname = function_name{"__main__", no_struct, "$main"};
    com.functions.emplace_back(fname, 0, variable_manager{false});

    com.current_function.emplace_back(0);
    com.current_struct.emplace_back(fname.struct_name);
    com.current_module.emplace_back(fname.module);
    variables(com).new_scope();
    push_stmt(com, *ast.root);
    variables(com).pop_scope(code(com));
    com.current_module.pop_back();
    com.current_struct.pop_back();
    com.current_function.pop_back();

    push_value(com.functions[0].code, op::end_program);

    auto program = bytecode_program{};
    program.rom = com.rom;
    for (const auto& function : com.functions) {
        program.functions.push_back(bytecode_function{function.name.to_string(), function.id, function.code});
    }
    return program;
}

}
