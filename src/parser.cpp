#include "parser.hpp"
#include "object.hpp"
#include "functions.hpp"
#include "lexer.hpp"
#include "utility/common.hpp"

#include <string_view>
#include <vector>
#include <memory>
#include <charconv>

namespace anzu {
namespace {

template <typename ExprType, token_type TokenType>
auto parse_number(const token& tok) -> node_expr_ptr
{
    tok.assert_type(TokenType, "");
    auto node = std::make_shared<node_expr>();
    auto& inner = node->emplace<ExprType>();
    inner.token = tok;
    auto text = tok.text;

    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), inner.value);
    tok.assert(ec == std::errc{}, "cannot convert '{}' to '{}'\n", text, TokenType);
    return node;
}

auto parse_i32(const token& tok) -> node_expr_ptr
{
    return parse_number<node_literal_i32_expr, token_type::int32>(tok);
}

auto parse_i64(const token& tok) -> node_expr_ptr
{
    return parse_number<node_literal_i64_expr, token_type::int64>(tok);
}

auto parse_u64(const token& tok) -> node_expr_ptr
{
    return parse_number<node_literal_u64_expr, token_type::uint64>(tok);
}

auto parse_f64(const token& tok) -> node_expr_ptr
{
    return parse_number<node_literal_f64_expr, token_type::float64>(tok);
}

auto parse_char(const token& tok) -> node_expr_ptr
{
    tok.assert_type(token_type::character, "");
    auto node = std::make_shared<node_expr>();
    auto& inner = node->emplace<node_literal_char_expr>();
    inner.token = tok;
    inner.value = tok.text.front();
    return node;
}

auto parse_string(const token& tok) -> node_expr_ptr
{
    tok.assert_type(token_type::string, "");
    auto node = std::make_shared<node_expr>();
    auto& inner = node->emplace<node_literal_string_expr>();
    inner.token = tok;
    inner.value = tok.text;
    return node;
}

auto parse_bool(const token& tok) -> node_expr_ptr
{
    auto node = std::make_shared<node_expr>();
    auto& inner = node->emplace<node_literal_bool_expr>();
    inner.token = tok;
    switch (tok.type) {
        case token_type::kw_true:  { inner.value = true;  } break;
        case token_type::kw_false: { inner.value = false; } break;
        default: tok.error("cannot parse bool literal from {}\n", tok.type);
    }
    return node;
}

auto parse_null(const token& tok) -> node_expr_ptr
{
    tok.assert_type(token_type::kw_null, "cannot parse null literal from {}\n", tok.type);
    auto node = std::make_shared<node_expr>();
    auto& inner = node->emplace<node_literal_null_expr>();
    inner.token = tok;
    return node;
}

auto parse_expression(tokenstream& tokens) -> node_expr_ptr;
auto parse_statement(tokenstream& tokens) -> node_stmt_ptr;
auto parse_type(tokenstream& tokens) -> type_name;
auto parse_type_node(tokenstream& tokens) -> node_type_ptr;

auto parse_literal(tokenstream& tokens) -> node_expr_ptr
{
    const auto token = tokens.consume();
    switch (token.type) {
        case token_type::int32:     return parse_i32(token);
        case token_type::int64:     return parse_i64(token);
        case token_type::uint64:    return parse_u64(token);
        case token_type::float64:   return parse_f64(token);
        case token_type::character: return parse_char(token);
        case token_type::kw_true:   return parse_bool(token);
        case token_type::kw_false:  return parse_bool(token);
        case token_type::kw_null:   return parse_null(token);
        case token_type::string:    return parse_string(token);
    }
    token.error("failed to parse literal ({})", token.text);
};

static constexpr auto prec_none = 0;
static constexpr auto prec_or = 1;
static constexpr auto prec_and = 2;
static constexpr auto prec_equality = 3;
static constexpr auto prec_comparison = 4;
static constexpr auto prec_term = 5;
static constexpr auto prec_factor = 6;
static constexpr auto prec_unit = 7;

auto get_precedence(token token) -> int
{
    switch (token.type) {
        case token_type::bar_bar:             return prec_or;
        case token_type::ampersand_ampersand: return prec_and;
        case token_type::equal_equal:
        case token_type::bang_equal:          return prec_equality;
        case token_type::less:
        case token_type::less_equal:
        case token_type::greater:
        case token_type::greater_equal:       return prec_comparison;
        case token_type::plus:
        case token_type::minus:               return prec_term;
        case token_type::star:
        case token_type::slash:
        case token_type::percent:             return prec_factor;
        default:                              return prec_none;
    }
}

auto parse_name(tokenstream& tokens)
{
    const auto token = tokens.consume();
    if (token.type != token_type::identifier) {
        token.error("'{}' is not a valid name", token.text);
    }
    return token.text;
}

auto parse_member_access(tokenstream& tokens, node_expr_ptr& node)
{
    auto new_node = std::make_shared<node_expr>();
    const auto tok = tokens.consume();
    if (tokens.peek_next(token_type::left_paren)) {
        auto& expr = new_node->emplace<node_member_call_expr>();
        expr.expr = node;
        expr.token = tok;
        expr.function_name = parse_name(tokens);
        tokens.consume_only(token_type::left_paren);
        tokens.consume_comma_separated_list(token_type::right_paren, [&] {
            expr.other_args.push_back(parse_expression(tokens));
        });
    } else {
        auto& expr = new_node->emplace<node_field_expr>();
        expr.token = tok;
        expr.field_name = tokens.consume().text;
        expr.expr = node;
    }
    node = new_node;
}

auto parse_single_factor(tokenstream& tokens) -> node_expr_ptr
{
    auto node = std::make_shared<node_expr>();

    switch (tokens.curr().type) {
        case token_type::left_paren: {
            tokens.consume();
            node = parse_expression(tokens);
            tokens.consume_only(token_type::right_paren);
        } break;
        case token_type::left_bracket: {
            const auto tok = tokens.consume();
            auto first = parse_expression(tokens);
            if (tokens.consume_maybe(token_type::semicolon)) {
                auto& expr = node->emplace<node_repeat_array_expr>();
                expr.token = tok;
                expr.value = first;
                expr.size = tokens.consume_u64();
                tokens.consume_only(token_type::right_bracket);
            } else {
                auto& expr = node->emplace<node_array_expr>();
                expr.token = tok;
                expr.elements.push_back(first);
                if (tokens.consume_maybe(token_type::comma)) {
                    tokens.consume_comma_separated_list(token_type::right_bracket, [&] {
                        expr.elements.push_back(parse_expression(tokens));
                    });
                } else {
                    tokens.consume_only(token_type::right_bracket);
                }
            }
        } break;
        case token_type::minus:
        case token_type::bang: {
            auto& expr = node->emplace<node_unary_op_expr>();
            expr.token = tokens.consume();
            expr.expr = parse_single_factor(tokens);
        } break;
        case token_type::kw_sizeof: {
            auto& expr = node->emplace<node_sizeof_expr>();
            expr.token = tokens.consume();
            tokens.consume_only(token_type::left_paren);
            expr.expr = parse_expression(tokens);
            tokens.consume_only(token_type::right_paren);
        } break;
        case token_type::identifier: {
            auto& expr = node->emplace<node_name_expr>();
            expr.token = tokens.consume();
            expr.name = expr.token.text;
        } break;
        case token_type::kw_new: {
            auto& expr = node->emplace<node_new_expr>();
            expr.token = tokens.consume();
            expr.type = parse_type_node(tokens);
            if (tokens.consume_maybe(token_type::colon)) {
                expr.size = parse_expression(tokens);
            } else {
                expr.size = nullptr;
            }
        } break;
        default: {
            node = parse_literal(tokens);
        } break;
    }

    // Handle postfix expressions
    while (true) {
        switch (tokens.curr().type) {
            case token_type::at: {
                auto new_node = std::make_shared<node_expr>();
                auto& inner = new_node->emplace<node_deref_expr>();
                inner.token = tokens.consume();
                inner.expr = node;
                node = new_node;
            } break;
            case token_type::ampersand: {
                auto new_node = std::make_shared<node_expr>();
                auto& inner = new_node->emplace<node_addrof_expr>();
                inner.token = tokens.consume();
                inner.expr = node;
                node = new_node;
            } break;
            case token_type::dot: {
                parse_member_access(tokens, node);
            } break;
            case token_type::left_bracket: { // subscript or span
                const auto token = tokens.consume();
                auto new_node = std::make_shared<node_expr>();
                if (tokens.consume_maybe(token_type::right_bracket)) {
                    auto& expr = new_node->emplace<node_span_expr>();
                    expr.token = token;
                    expr.expr = node;
                    node = new_node;
                } else { // either a subspan or subscript access
                    const auto inner_expr = parse_expression(tokens);
                    if (tokens.consume_maybe(token_type::colon)) { // subspan
                        auto& expr = new_node->emplace<node_span_expr>();
                        expr.token = token;
                        expr.expr = node;
                        expr.lower_bound = inner_expr;
                        expr.upper_bound = parse_expression(tokens);
                        node = new_node;
                    } else { // subscript access
                        auto& expr = new_node->emplace<node_subscript_expr>();
                        expr.token = token;
                        expr.index = inner_expr;
                        expr.expr = node;
                        node = new_node;
                    }
                    tokens.consume_only(token_type::right_bracket);
                }
            } break;
            case token_type::left_paren: { // callable expressions
                auto new_node = std::make_shared<node_expr>();
                auto& inner = new_node->emplace<node_call_expr>();
                inner.token = tokens.consume();
                inner.expr = node;
                tokens.consume_comma_separated_list(token_type::right_paren, [&] {
                    inner.args.push_back(parse_expression(tokens));
                });
                node = new_node;
            } break;
            default: {
                return node;
            } break;
        }
    }
}

auto parse_compound_factor(tokenstream& tokens, int level) -> node_expr_ptr
{
    if (level == prec_unit) {
        return parse_single_factor(tokens);
    }

    auto factor = parse_compound_factor(tokens, level + 1);
    while (level < get_precedence(tokens.curr())) {
        auto node = std::make_shared<node_expr>();
        auto& expr = node->emplace<node_binary_op_expr>();
        expr.lhs = factor;
        expr.token = tokens.consume();
        expr.rhs = parse_compound_factor(tokens, level + 1);
        factor = node;
    }
    return factor;
}

auto parse_expression(tokenstream& tokens) -> node_expr_ptr
{
    return parse_compound_factor(tokens, 0);
}

// If it's a fundamental type, return that, otherwise return a struct_type
auto parse_simple_type(tokenstream& tokens) -> type_name
{
    const auto tok = tokens.consume();
    if (tok.text == "null") return type_fundamental::null_type;
    if (tok.text == "bool") return type_fundamental::bool_type;
    if (tok.text == "char") return type_fundamental::char_type;
    if (tok.text == "i32")  return type_fundamental::i32_type;
    if (tok.text == "i64")  return type_fundamental::i64_type;
    if (tok.text == "u64")  return type_fundamental::u64_type;
    if (tok.text == "f64")  return type_fundamental::f64_type;
    return {type_struct{ .name=std::string{tok.text} }};
}

auto parse_type_inner(tokenstream& tokens) -> type_name
{
    // Const
    if (tokens.consume_maybe(token_type::kw_const)) {
        auto ret = type_const{};
        ret.inner_type = make_value<type_name>(parse_type_inner(tokens));
        return ret;
    }

    // Function pointers
    if (tokens.consume_maybe(token_type::kw_function)) {
        tokens.consume_only(token_type::left_paren);
        auto ret = type_function_ptr{};
        tokens.consume_comma_separated_list(token_type::right_paren, [&]{
            ret.param_types.push_back(parse_type_inner(tokens));
        });
        tokens.consume_only(token_type::arrow);
        ret.return_type = make_value<type_name>(parse_type_inner(tokens));
        return ret;
    }

    auto type = null_type();
    if (tokens.consume_maybe(token_type::left_paren)) {
        type = parse_type_inner(tokens);
        tokens.consume_only(token_type::right_paren);
    } else {
        type = parse_simple_type(tokens);
    }

    while (true) {
        if (tokens.consume_maybe(token_type::left_bracket)) {
            if (tokens.consume_maybe(token_type::right_bracket)) {
                type = type_name{type_span{ .inner_type=type }};
            }
            else {
                type = type_name{type_array{ .inner_type=type, .count=tokens.consume_u64() }};
                tokens.consume_only(token_type::right_bracket);
            }
        }
        else if (tokens.consume_maybe(token_type::ampersand)) {
            type = type_name{type_ptr{ .inner_type=type }};
        }
        else {
            break;
        }
    }
    return type;
}

auto validate_type_inner(const type_name& type) -> std::optional<std::string_view>
{
    using Ret = std::optional<std::string_view>;
    return std::visit(overloaded{
        [](type_fundamental) { return Ret{}; },
        [](const type_struct&) { return Ret{}; },
        [](const type_array& t) { return validate_type_inner(*t.inner_type); },
        [](const type_ptr& t) { return validate_type_inner(*t.inner_type); },
        [](const type_span& t) { return validate_type_inner(*t.inner_type); },
        [](const type_function_ptr& t) {
            for (const auto& param : t.param_types) {
                const auto err = validate_type_inner(param);
                if (err) {
                    return Ret{"invalid function param of fn ptr type"};
                }
            }
            const auto err = validate_type_inner(*t.return_type);
            if (err) {
                return Ret{"invalid return type of fn ptr type"};
            }
            return Ret{};
        },
        [](const type_const& t) { return validate_type_inner(*t.inner_type); }
    }, type);
}

// Parses a type, then checks it's valid (no internal references)
auto parse_type(tokenstream& tokens) -> type_name
{
    const auto token = tokens.curr();
    const auto type = parse_type_inner(tokens);
    const auto err = validate_type_inner(type);
    if (err) {
        token.error("Invalid type ({}) - {}", type, *err);
    }
    return type;
}

auto parse_type_node(tokenstream& tokens) -> node_type_ptr
{
    if (tokens.peek(token_type::kw_typeof)) {
        auto node = std::make_shared<node_type>();
        auto& inner = node->emplace<node_expr_type>();
        inner.token = tokens.consume();
        tokens.consume_only(token_type::left_paren);
        inner.expr = parse_expression(tokens);
        tokens.consume_only(token_type::right_paren);
        return node;
    }

    const auto type = parse_type(tokens);
    return std::make_shared<node_type>(node_named_type{type});
}

auto parse_function_def_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_function_def_stmt>();

    stmt.token = tokens.consume_only(token_type::kw_function);
    stmt.name = parse_name(tokens);
    tokens.consume_only(token_type::left_paren);
    tokens.consume_comma_separated_list(token_type::right_paren, [&]{
        auto param = node_parameter{};
        param.name = parse_name(tokens);
        tokens.consume_only(token_type::colon);
        param.type = parse_type_node(tokens);
        stmt.sig.params.push_back(param);
    });
    if (tokens.consume_maybe(token_type::arrow)) {
        stmt.sig.return_type = parse_type_node(tokens);
    } else {
        stmt.sig.return_type = std::make_shared<node_type>(node_named_type{null_type()});
    }
    stmt.body = parse_statement(tokens);
    return node;
}

auto parse_member_function_def_stmt(const std::string& struct_name, tokenstream& tokens)
    -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_member_function_def_stmt>();

    stmt.token = tokens.consume_only(token_type::kw_function);
    stmt.struct_name = struct_name;
    stmt.function_name = parse_name(tokens);

    tokens.consume_only(token_type::left_paren);
    tokens.consume_comma_separated_list(token_type::right_paren, [&]{
        auto param = node_parameter{};
        param.name = parse_name(tokens);
        tokens.consume_only(token_type::colon);
        param.type = parse_type_node(tokens);
        stmt.sig.params.push_back(param);
    });
    if (tokens.consume_maybe(token_type::arrow)) {
        stmt.sig.return_type = parse_type_node(tokens);
    } else {
        stmt.sig.return_type = std::make_shared<node_type>(node_named_type{null_type()});
    }
    stmt.body = parse_statement(tokens);
    return node;
}

auto parse_return_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_return_stmt>();

    stmt.token = tokens.consume_only(token_type::kw_return);
    if (tokens.peek(token_type::semicolon)) {
        stmt.return_value = std::make_shared<node_expr>();
        auto& ret_expr = stmt.return_value->emplace<node_literal_null_expr>();
        ret_expr.token = stmt.token;
    } else {
        stmt.return_value = parse_expression(tokens);
    }
    tokens.consume_only(token_type::semicolon);
    return node;
}

auto parse_loop_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_loop_stmt>();

    stmt.token = tokens.consume_only(token_type::kw_loop);
    stmt.body = parse_statement(tokens);
    return node;
}

auto parse_while_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_while_stmt>();

    stmt.token = tokens.consume_only(token_type::kw_while);
    stmt.condition = parse_expression(tokens);
    stmt.body = parse_statement(tokens);
    return node;
}

auto parse_for_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_for_stmt>();

    stmt.token = tokens.consume_only(token_type::kw_for);
    stmt.name = parse_name(tokens);
    tokens.consume_only(token_type::kw_in);
    stmt.iter = parse_expression(tokens);
    stmt.body = parse_statement(tokens);
    return node;
}

auto parse_if_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_if_stmt>();

    stmt.token = tokens.consume_only(token_type::kw_if);
    stmt.condition = parse_expression(tokens);
    stmt.body = parse_statement(tokens);
    if (tokens.consume_maybe(token_type::kw_else)) {
        stmt.else_body = parse_statement(tokens);
    }
    return node;
}

auto parse_struct_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_struct_stmt>();

    stmt.token = tokens.consume_only(token_type::kw_struct);
    stmt.name = parse_name(tokens);
    tokens.consume_only(token_type::left_brace);
    while (!tokens.consume_maybe(token_type::right_brace)) {
        if (tokens.peek(token_type::kw_function)) {
            stmt.functions.emplace_back(parse_member_function_def_stmt(stmt.name, tokens));
        } else {
            stmt.fields.emplace_back();
            auto& f = stmt.fields.back();
            f.name = parse_name(tokens);
            tokens.consume_only(token_type::colon);
            f.type = parse_type_node(tokens);
            tokens.consume_only(token_type::semicolon);
        }
    }

    return node;
}

auto parse_declaration_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_declaration_stmt>();

    stmt.token = tokens.consume();

    switch (stmt.token.type) {
        case token_type::kw_let: { stmt.add_const = true; } break;
        case token_type::kw_var: { stmt.add_const = false; } break;
        default: stmt.token.error("declaration must start with 'let' or 'var', not {}",
                                  stmt.token.text);
    }

    stmt.name = parse_name(tokens);
    tokens.consume_only(token_type::colon_equal);
    stmt.expr = parse_expression(tokens);
    tokens.consume_only(token_type::semicolon);
    return node;
}

auto parse_print_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_print_stmt>();

    stmt.token = tokens.consume_only(token_type::kw_print);
    tokens.consume_only(token_type::left_paren);
    const auto message_token = tokens.consume_only(token_type::string);
    stmt.message = std::string{message_token.text};
    if (tokens.consume_maybe(token_type::comma)) {
        tokens.consume_comma_separated_list(token_type::right_paren, [&] {
            stmt.args.push_back(parse_expression(tokens));
        });
    } else {
        tokens.consume_only(token_type::right_paren);
    }
    return node;
}

auto parse_braced_statement_list(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_sequence_stmt>();

    stmt.token = tokens.consume_only(token_type::left_brace);
    while (!tokens.consume_maybe(token_type::right_brace)) {
        stmt.sequence.push_back(parse_statement(tokens));
    }

    return node;
}

auto parse_delete_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_delete_stmt>();

    stmt.token = tokens.consume_only(token_type::kw_delete);
    stmt.expr = parse_expression(tokens);
    tokens.consume_only(token_type::semicolon);

    return node;
}

auto parse_assert_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_assert_stmt>();

    stmt.token = tokens.consume_only(token_type::kw_assert);
    stmt.expr = parse_expression(tokens);
    tokens.consume_only(token_type::semicolon);
    return node;
}

auto parse_break_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto ret = std::make_shared<node_stmt>(node_break_stmt{ tokens.consume() });
    tokens.consume_only(token_type::semicolon);
    return ret;
}

auto parse_continue_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto ret = std::make_shared<node_stmt>(node_continue_stmt{ tokens.consume() });
    tokens.consume_only(token_type::semicolon);
    return ret;
}

auto parse_statement(tokenstream& tokens) -> node_stmt_ptr
{
    const auto drain_semicolons = scope_exit([&] {
        while (tokens.consume_maybe(token_type::semicolon));
    });

    const auto& curr = tokens.curr();
    switch (curr.type) {
        case token_type::kw_function: curr.error("functions can only exist in global scope");
        case token_type::kw_struct:   curr.error("structs can only exist in global scope");
        case token_type::kw_return:   return parse_return_stmt(tokens);
        case token_type::kw_loop:     return parse_loop_stmt(tokens);
        case token_type::kw_while:    return parse_while_stmt(tokens);
        case token_type::kw_for:      return parse_for_stmt(tokens);
        case token_type::kw_if:       return parse_if_stmt(tokens);
        case token_type::kw_delete:   return parse_delete_stmt(tokens);
        case token_type::kw_assert:   return parse_assert_stmt(tokens);
        case token_type::kw_break:    return parse_break_stmt(tokens);
        case token_type::kw_continue: return parse_continue_stmt(tokens);
        case token_type::left_brace:  return parse_braced_statement_list(tokens);
        case token_type::kw_let:
        case token_type::kw_var:      return parse_declaration_stmt(tokens);
        case token_type::kw_print:    return parse_print_stmt(tokens);
    }

    auto node = std::make_shared<node_stmt>();
    auto expr = parse_expression(tokens);
    if (tokens.peek(token_type::equal)) {
        auto& stmt = node->emplace<node_assignment_stmt>();
        stmt.token = tokens.consume();
        stmt.position = expr;
        stmt.expr = parse_expression(tokens);
    } else {
        auto& stmt = node->emplace<node_expression_stmt>();
        stmt.token = std::visit([](auto&& n) { return n.token; }, *expr);
        stmt.expr = expr;
    }
    tokens.consume_only(token_type::semicolon);
    return node;
}

auto parse_top_level_statement(tokenstream& tokens) -> node_stmt_ptr
{
    const auto drain_semicolons = scope_exit([&] {
        while (tokens.consume_maybe(token_type::semicolon));
    });
    if (!tokens.valid()) return nullptr;

    const auto& curr = tokens.curr();
    switch (curr.type) {
        case token_type::kw_function: return parse_function_def_stmt(tokens);
        case token_type::kw_struct:   return parse_struct_stmt(tokens);
        default:                      return parse_statement(tokens);
    }
}

}

auto parse(const std::filesystem::path& file) -> anzu_module
{
    auto new_module = anzu_module{};
    new_module.source_code = anzu::read_file(file);
    new_module.root = std::make_shared<node_stmt>();
    auto& seq = new_module.root->emplace<node_sequence_stmt>();

    auto stream = tokenstream{*new_module.source_code};
    while (stream.valid()) {
        while (stream.consume_maybe(token_type::semicolon));
        if (stream.consume_maybe(token_type::kw_import)) {
            auto module_name = std::string{};
            while (!stream.peek(token_type::semicolon)) {
                module_name += stream.consume().text;
            }
            new_module.required_modules.emplace(
                std::filesystem::absolute(file.parent_path() / module_name)
            );
            stream.consume_only(token_type::semicolon);
        } else {
            seq.sequence.push_back(parse_top_level_statement(stream));
        }
    }
    return new_module;
}

}