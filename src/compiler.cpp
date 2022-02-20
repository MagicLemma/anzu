#include "compiler.hpp"
#include "lexer.hpp"
#include "object.hpp"
#include "parser.hpp"
#include "functions.hpp"
#include "utility/print.hpp"

#include <string_view>
#include <optional>
#include <tuple>
#include <vector>
#include <unordered_map>

namespace anzu {

struct compiler_scope
{
    struct function_def
    {
        signature     sig;
        std::intptr_t ptr;
    };

    std::unordered_map<std::string, function_def> functions;
    std::unordered_map<std::string, std::size_t>  variables;
};

// Struct used to store information while compiling an AST. Contains the output program
// as well as information such as function definitions.
struct compiler_context
{
    anzu::program program;
    std::vector<compiler_scope> scopes;
};

// Registers the given name in the current scope
auto declare_variable_name(compiler_context& ctx, const std::string& name) -> void
{
    auto& vars = ctx.scopes.back().variables;
    vars.emplace(name, vars.size());
}

auto save_variable(compiler_context& ctx, const std::string& name) -> void
{
    declare_variable_name(ctx, name);
    ctx.program.emplace_back(anzu::op_save_variable{
        .name=name, .offset=ctx.scopes.back().variables.at(name)
    });
}

auto load_variable(compiler_context& ctx, const std::string& name) -> void
{
    for (const auto& scope : ctx.scopes | std::views::reverse) {
        if (auto it = scope.variables.find(name); it != scope.variables.end()) {
            ctx.program.emplace_back(anzu::op_load_variable{
                .name=name, .offset=it->second
            });
            return;
        }
    }
}

auto call_builtin(compiler_context& ctx, const std::string& function_name) -> void
{
    const auto& func = anzu::fetch_builtin(function_name);
    ctx.program.emplace_back(anzu::op_builtin_call{
        .name=function_name, .ptr=func.ptr, .sig=func.sig
    });
}

auto find_function(
    const compiler_context& ctx, const std::string& function
)
    -> const compiler_scope::function_def*
{
    for (const auto& scope : ctx.scopes | std::views::reverse) {
        if (const auto it = scope.functions.find(function); it != scope.functions.end()) {
            return &it->second;
        }
    }
    return nullptr;
}

namespace {

// Both for and while loops have the form [<begin> <condition> <do> <body> <end>].
// This function links the do to jump to one past the end if false, makes breaks
// jump past the end, and makes continues jump back to the beginning.
auto link_up_jumps(
    compiler_context& ctx,
    std::intptr_t loop_begin,
    std::intptr_t loop_do,
    std::intptr_t loop_end
)
    -> void
{
    // Jump past the end if false
    std::get<anzu::op_jump_if_false>(ctx.program[loop_do]).jump = loop_end + 1;
        
    // Only set unset jumps, there may be other already set from nested loops
    for (std::intptr_t idx = loop_do + 1; idx != loop_end; ++idx) {
        if (auto op = std::get_if<anzu::op_break>(&ctx.program[idx]); op && op->jump == -1) {
            op->jump = loop_end + 1;
        }
        else if (auto op = std::get_if<anzu::op_continue>(&ctx.program[idx]); op && op->jump == -1) {
            op->jump = loop_begin;
        }
    }
}

auto compile_node(const node_expr& root, compiler_context& ctx) -> void;
auto compile_node(const node_stmt& root, compiler_context& ctx) -> void;

void compile_function_call(
    const std::string& function,
    const std::vector<node_expr_ptr>& args,
    compiler_context& ctx
)
{
    // Push the args to the stack
    for (const auto& arg : args) {
        compile_node(*arg, ctx);
    }

    if (const auto function_def = find_function(ctx, function)) {
        ctx.program.emplace_back(anzu::op_function_call{
            .name=function,
            .ptr=function_def->ptr + 1, // Jump into the function
            .sig=function_def->sig
        });
    }
    else {
        const auto& builtin = anzu::fetch_builtin(function);
        ctx.program.emplace_back(anzu::op_builtin_call{
            .name=function,
            .ptr=builtin.ptr,
            .sig=builtin.sig
        });
    }
}

void compile_node(const node_literal_expr& node, compiler_context& ctx)
{
    ctx.program.emplace_back(anzu::op_load_literal{ .value=node.value });
}

void compile_node(const node_variable_expr& node, compiler_context& ctx)
{
    load_variable(ctx, node.name);
}

void compile_node(const node_bin_op_expr& node, compiler_context& ctx)
{
    compile_node(*node.lhs, ctx);
    compile_node(*node.rhs, ctx);
    const auto op = node.token.text;
    if      (op == "+")  { ctx.program.emplace_back(anzu::op_add{}); }
    else if (op == "-")  { ctx.program.emplace_back(anzu::op_sub{}); }
    else if (op == "*")  { ctx.program.emplace_back(anzu::op_mul{}); }
    else if (op == "/")  { ctx.program.emplace_back(anzu::op_div{}); }
    else if (op == "%")  { ctx.program.emplace_back(anzu::op_mod{}); }
    else if (op == "<")  { ctx.program.emplace_back(anzu::op_lt{}); }
    else if (op == "<=") { ctx.program.emplace_back(anzu::op_le{}); }
    else if (op == ">")  { ctx.program.emplace_back(anzu::op_gt{}); }
    else if (op == ">=") { ctx.program.emplace_back(anzu::op_ge{}); }
    else if (op == "==") { ctx.program.emplace_back(anzu::op_eq{}); }
    else if (op == "!=") { ctx.program.emplace_back(anzu::op_ne{}); }
    else if (op == "||") { ctx.program.emplace_back(anzu::op_or{}); }
    else if (op == "&&") { ctx.program.emplace_back(anzu::op_and{}); }
    else {
        anzu::print("syntax error: unknown binary operator: '{}'\n", op);
        std::exit(1);
    }
}

void compile_node(const node_function_call_expr& node, compiler_context& ctx)
{
    compile_function_call(node.function_name, node.args, ctx);
}

void compile_node(const node_list_expr& node, compiler_context& ctx)
{
    for (const auto& element : node.elements | std::views::reverse) {
        compile_node(*element, ctx);
    }
    ctx.program.emplace_back(anzu::op_build_list{ .size = node.elements.size() });
}

void compile_node(const node_sequence_stmt& node, compiler_context& ctx)
{
    for (const auto& seq_node : node.sequence) {
        compile_node(*seq_node, ctx);
    }
}

void compile_node(const node_while_stmt& node, compiler_context& ctx)
{
    const auto while_pos = std::ssize(ctx.program);
    ctx.program.emplace_back(anzu::op_loop_begin{});

    compile_node(*node.condition, ctx);
    
    const auto do_pos = std::ssize(ctx.program);
    ctx.program.emplace_back(anzu::op_jump_if_false{});

    compile_node(*node.body, ctx);

    const auto end_pos = std::ssize(ctx.program);
    ctx.program.emplace_back(anzu::op_loop_end{ .jump=while_pos }); // Jump back to start

    link_up_jumps(ctx, while_pos, do_pos, end_pos);
}

void compile_node(const node_if_stmt& node, compiler_context& ctx)
{
    const auto if_pos = std::ssize(ctx.program);
    ctx.program.emplace_back(anzu::op_if{});

    compile_node(*node.condition, ctx);
    
    const auto do_pos = std::ssize(ctx.program);
    ctx.program.emplace_back(anzu::op_jump_if_false{});

    compile_node(*node.body, ctx);

    auto else_pos = std::intptr_t{-1};
    if (node.else_body) {
        else_pos = std::ssize(ctx.program);
        ctx.program.emplace_back(anzu::op_else{});
        compile_node(*node.else_body, ctx);
    }

    ctx.program.emplace_back(anzu::op_if_end{});
    if (else_pos == -1) {
        std::get<anzu::op_jump_if_false>(ctx.program[do_pos]).jump = std::ssize(ctx.program); // Jump past the end if false
    } else {
        std::get<anzu::op_jump_if_false>(ctx.program[do_pos]).jump = else_pos + 1; // Jump into the else block if false
        std::get<anzu::op_else>(ctx.program[else_pos]).jump = std::ssize(ctx.program); // Jump past the end if false
    }
}

void compile_node(const node_for_stmt& node, compiler_context& ctx)
{
    const auto container_name = std::string{"_Container"};
    const auto index_name = std::string{"_Index"};

    // Push the container to the stack
    compile_node(*node.container, ctx);
    save_variable(ctx, container_name);

    // Push the counter to the stack
    ctx.program.emplace_back(anzu::op_load_literal{ .value=object{0} });
    save_variable(ctx, index_name);

    const auto begin_pos = std::ssize(ctx.program);
    ctx.program.emplace_back(anzu::op_loop_begin{});

    load_variable(ctx, index_name);
    load_variable(ctx, container_name);
    call_builtin(ctx, "list_size");
    ctx.program.emplace_back(anzu::op_ne{});
    
    const auto do_pos = std::ssize(ctx.program);
    ctx.program.emplace_back(anzu::op_jump_if_false{});   // If size == index, jump to end

    load_variable(ctx, container_name);
    load_variable(ctx, index_name);
    call_builtin(ctx, "list_at");
    save_variable(ctx, node.var);

    compile_node(*node.body, ctx);

    // Increment the index
    load_variable(ctx, index_name);
    ctx.program.emplace_back(anzu::op_load_literal{ .value=object{1} });
    ctx.program.emplace_back(anzu::op_add{});
    save_variable(ctx, index_name);

    const auto end_pos = std::ssize(ctx.program);
    ctx.program.emplace_back(anzu::op_loop_end{ .jump=begin_pos });

    link_up_jumps(ctx, begin_pos, do_pos, end_pos);
}

void compile_node(const node_break_stmt&, compiler_context& ctx)
{
    ctx.program.emplace_back(anzu::op_break{});
}

void compile_node(const node_continue_stmt&, compiler_context& ctx)
{
    ctx.program.emplace_back(anzu::op_continue{});
}

void compile_node(const node_declaration_stmt& node, compiler_context& ctx)
{
    compile_node(*node.expr, ctx);
    save_variable(ctx, node.name);
}

void compile_node(const node_assignment_stmt& node, compiler_context& ctx)
{
    compile_node(*node.expr, ctx);
    save_variable(ctx, node.name);
}

void compile_node(const node_function_def_stmt& node, compiler_context& ctx)
{
    const auto start_pos = std::ssize(ctx.program);
    ctx.program.emplace_back(anzu::op_function{ .name=node.name, .sig=node.sig });
    ctx.scopes.back().functions[node.name] = { .sig=node.sig ,.ptr=start_pos };

    ctx.scopes.emplace_back();
    for (const auto& arg : node.sig.args) {
        declare_variable_name(ctx, arg.name);
    }
    compile_node(*node.body, ctx);
    ctx.scopes.pop_back();

    const auto end_pos = std::ssize(ctx.program);
    ctx.program.emplace_back(anzu::op_function_end{});

    std::get<anzu::op_function>(ctx.program[start_pos]).jump = end_pos + 1;
}

void compile_node(const node_function_call_stmt& node, compiler_context& ctx)
{
    compile_function_call(node.function_name, node.args, ctx);
    ctx.program.emplace_back(anzu::op_pop{});
}

void compile_node(const node_return_stmt& node, compiler_context& ctx)
{
    compile_node(*node.return_value, ctx);
    ctx.program.emplace_back(anzu::op_return{});
}

auto compile_node(const node_expr& root, compiler_context& ctx) -> void
{
    std::visit([&](const auto& node) { compile_node(node, ctx); }, root);
}

auto compile_node(const node_stmt& root, compiler_context& ctx) -> void
{
    std::visit([&](const auto& node) { compile_node(node, ctx); }, root);
}

}

auto compile(const std::unique_ptr<node_stmt>& root) -> anzu::program
{
    anzu::compiler_context ctx;
    ctx.scopes.emplace_back(); // Global scope
    compile_node(*root, ctx);
    return ctx.program;
}

}