#include "parser.hpp"
#include "object.hpp"
#include "lexer.hpp"
#include "utility/common.hpp"

#include <string_view>
#include <vector>
#include <memory>
#include <charconv>

namespace anzu {
namespace {

auto parse_expression(tokenstream& tokens) -> node_expr_ptr;
auto parse_statement(tokenstream& tokens) -> node_stmt_ptr;
auto parse_identifier(tokenstream& tokens) -> std::string;

enum class precedence {
  none,
  as,          // as
  ternary,     // ?:
  logical_or,  // or
  logical_and, // and
  equality,    // == !=
  comparison,  // < > <= >=
  term,        // + -
  factor,      // * /
  unary,       // ! -
  call,        // . () [] !() @ const &
  scope,       // ::
  primary
};

using prefix_func = auto(*) (tokenstream&) -> node_expr_ptr;
using midfix_func = auto(*) (tokenstream&, const node_expr_ptr&) -> node_expr_ptr;

struct parse_rule
{
    prefix_func prefix;
    midfix_func midfix;
    precedence  prec;
};

auto parse_name_pack(tokenstream& tokens) -> name_pack
{
    name_pack np;
    if (tokens.consume_maybe(token_type::left_bracket)) {
        auto& names = np.names.emplace<std::vector<name_pack>>();
        tokens.consume_comma_separated_list(token_type::right_bracket, [&] {
            names.push_back(parse_name_pack(tokens));
        });
    } else {
        np.names = parse_identifier(tokens);
    }
    return np;
}

template <typename Inner>
auto new_node(const token& tok) -> std::tuple<node_expr_ptr, Inner&>
{
    auto node = std::make_shared<node_expr>();
    auto& inner = node->emplace<Inner>();
    inner.token = tok;
    return {node, std::ref(inner)};
}

auto parse_precedence(tokenstream& tokens, precedence prec) -> node_expr_ptr;
auto get_rule(token_type tt) -> const parse_rule*;

template <typename ExprType, token_type TokenType>
auto parse_number(tokenstream& tokens) -> node_expr_ptr
{
    const auto token = tokens.consume_only(TokenType);
    auto [node, inner] = new_node<ExprType>(token);
    auto text = token.text;

    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), inner.value);
    constexpr auto message = "cannot convert '{}' to '{}'\n";
    token.assert(ec == std::errc{}, message, text, TokenType);
    return node;
}

auto parse_i32(tokenstream& tokens) -> node_expr_ptr
{
    return parse_number<node_literal_i32_expr, token_type::int32>(tokens);
}

auto parse_i64(tokenstream& tokens) -> node_expr_ptr
{
    return parse_number<node_literal_i64_expr, token_type::int64>(tokens);
}

auto parse_u64(tokenstream& tokens) -> node_expr_ptr
{
    return parse_number<node_literal_u64_expr, token_type::uint64>(tokens);
}

auto parse_f64(tokenstream& tokens) -> node_expr_ptr
{
    return parse_number<node_literal_f64_expr, token_type::float64>(tokens);
}

auto parse_char(tokenstream& tokens) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::character);
    auto [node, inner] = new_node<node_literal_char_expr>(token);
    inner.value = token.text.front();
    return node;
}

auto parse_string(tokenstream& tokens) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::string);
    auto [node, inner] = new_node<node_literal_string_expr>(token);
    inner.value = token.text;
    return node;
}

auto parse_true(tokenstream& tokens) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::kw_true);
    auto [node, inner] = new_node<node_literal_bool_expr>(token);
    inner.value = true;
    return node;
}

auto parse_false(tokenstream& tokens) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::kw_false);
    auto [node, inner] = new_node<node_literal_bool_expr>(token);
    inner.value = false;
    return node;
}

auto parse_null(tokenstream& tokens) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::kw_null);
    auto [node, inner] = new_node<node_literal_null_expr>(token);
    return node;
}

auto parse_name(tokenstream& tokens) -> node_expr_ptr
{
    const auto token = tokens.consume();
    auto [node, inner] = new_node<node_name_expr>(token);
    inner.name = token.text;
    return node;
}

auto parse_grouping(tokenstream& tokens) -> node_expr_ptr
{
    tokens.consume_only(token_type::left_paren);
    const auto node = parse_expression(tokens);
    tokens.consume_only(token_type::right_paren);
    return node;
}

auto parse_unary(tokenstream& tokens) -> node_expr_ptr
{
    const auto op = tokens.consume();
    auto expr = parse_precedence(tokens, precedence::unary);
    auto [node, inner] = new_node<node_unary_op_expr>(op);
    inner.expr = expr;
    return node;
}

auto parse_array(tokenstream& tokens) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::left_bracket);
    const auto first = parse_expression(tokens);

    if (tokens.consume_maybe(token_type::semicolon)) {
        auto [node, inner] = new_node<node_repeat_array_expr>(token);
        inner.value = first;
        inner.size = std::get<node_literal_u64_expr>(*parse_u64(tokens)).value; // TODO: store the expr here
        tokens.consume_only(token_type::right_bracket);
        return node;
    } else {
        auto [node, inner] = new_node<node_array_expr>(token);
        inner.elements.push_back(first);
        if (!tokens.consume_maybe(token_type::right_bracket)) {
            tokens.consume_only(token_type::comma);
            tokens.consume_comma_separated_list(token_type::right_bracket, [&] {
                inner.elements.push_back(parse_expression(tokens));
            });
        }
        return node;
    }
}

auto parse_func_ptr(tokenstream& tokens) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::kw_function);
    tokens.consume_only(token_type::left_paren);
    auto [node, inner] = new_node<node_function_ptr_type_expr>(token);
    tokens.consume_comma_separated_list(token_type::right_paren, [&] {
        inner.params.push_back(parse_expression(tokens));
    });
    tokens.consume_only(token_type::arrow); // TODO: allow for this to be optional
    inner.return_type = parse_expression(tokens);
    return node;
}

auto parse_new(tokenstream& tokens) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::kw_new);
    tokens.consume_only(token_type::left_paren);
    auto [node, inner] = new_node<node_new_expr>(token);
    inner.arena = parse_expression(tokens);
    if (tokens.consume_maybe(token_type::comma)) {
        inner.count = parse_expression(tokens);
    }
    if (tokens.consume_maybe(token_type::comma)) {
        inner.original = parse_expression(tokens);
    }
    tokens.consume_only(token_type::right_paren);
    inner.expr = parse_expression(tokens);
    return node;
}

auto parse_intrinsic(tokenstream& tokens) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::at);
    auto [node, inner] = new_node<node_intrinsic_expr>(token);
    inner.name = parse_identifier(tokens);
    tokens.consume_only(token_type::left_paren);
    tokens.consume_comma_separated_list(token_type::right_paren, [&] {
        inner.args.push_back(parse_expression(tokens));
    });
    return node;
}

auto parse_binary(tokenstream& tokens, const node_expr_ptr& left) -> node_expr_ptr
{
    const auto op = tokens.consume();
    auto rule = get_rule(op.type);
    auto right = parse_precedence(tokens, precedence{std::to_underlying(rule->prec) + 1});

    auto [node, inner] = new_node<node_binary_op_expr>(op);
    inner.lhs = left;
    inner.rhs = right;
    return node;
}

auto parse_call(tokenstream& tokens, const node_expr_ptr& left) -> node_expr_ptr
{
    auto [node, inner] = new_node<node_call_expr>(tokens.curr());
    inner.expr = left;
    tokens.consume_only(token_type::left_paren);
    tokens.consume_comma_separated_list(token_type::right_paren, [&] {
        inner.args.push_back(parse_expression(tokens));
    });
    
    return node;
}

auto parse_templates(tokenstream& tokens, const node_expr_ptr& left) -> node_expr_ptr
{
    auto [node, inner] = new_node<node_template_expr>(tokens.consume());
    inner.expr = left;
    tokens.consume_only(token_type::left_paren);
    tokens.consume_comma_separated_list(token_type::right_paren, [&] {
        inner.templates.push_back(parse_expression(tokens));
    });
    
    return node;
}

auto parse_subscript(tokenstream& tokens, const node_expr_ptr& left) -> node_expr_ptr
{
    auto node = std::make_shared<node_expr>();
    const auto token = tokens.consume_only(token_type::left_bracket);

    if (tokens.consume_maybe(token_type::right_bracket)) {    // x[]
        auto& inner = node->emplace<node_span_expr>();
        inner.token = token;
        inner.expr = left;
        return node;
    }

    auto expr = parse_expression(tokens);
    if (tokens.consume_maybe(token_type::colon)) {            // x[a:b]
        auto& inner = node->emplace<node_span_expr>();
        inner.token = token;
        inner.expr = left;
        inner.lower_bound = expr;
        inner.upper_bound = parse_expression(tokens);
    } else {                                                  // x[i]
        auto& inner = node->emplace<node_subscript_expr>();
        inner.token = token;
        inner.expr = left;
        inner.index = expr;
    }

    tokens.consume_only(token_type::right_bracket);
    return node;
}

auto parse_dot(tokenstream& tokens, const node_expr_ptr& left) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::dot);
    auto [node, inner] = new_node<node_field_expr>(token);
    inner.expr = left;
    inner.name = parse_identifier(tokens);
    return node;
}

auto parse_const(tokenstream& tokens, const node_expr_ptr& left) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::kw_const);
    auto [node, inner] = new_node<node_const_expr>(token);
    inner.expr = left;
    return node;
}

auto parse_const_pre(tokenstream& tokens) -> node_expr_ptr
{
    return parse_const(tokens, nullptr);
}

auto parse_at(tokenstream& tokens, const node_expr_ptr& left) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::at);
    auto [node, inner] = new_node<node_deref_expr>(token);
    inner.expr = left;
    return node;
}

auto parse_ampersand(tokenstream& tokens, const node_expr_ptr& left) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::ampersand);
    auto [node, inner] = new_node<node_addrof_expr>(token);
    inner.expr = left;
    return node;
}

auto parse_ampersand_pre(tokenstream& tokens) -> node_expr_ptr
{
    return parse_ampersand(tokens, nullptr);
}

auto parse_question(tokenstream& tokens, const node_expr_ptr& left) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::question);
    auto [node, inner] = new_node<node_optional_expr>(token);
    inner.expr = left;
    return node;
}

auto parse_as(tokenstream& tokens, const node_expr_ptr& left) -> node_expr_ptr
{
    const auto token = tokens.consume_only(token_type::kw_as);
    auto [node, inner] = new_node<node_as_expr>(token);
    inner.expr = left;
    inner.type = parse_expression(tokens);
    return node;
}

auto parse_precedence(tokenstream& tokens, precedence prec) -> node_expr_ptr
{
    const auto token = tokens.curr();
    auto rule = get_rule(token.type);
    token.assert(rule->prefix, "expected an expression");

    auto node = rule->prefix(tokens);
    while (prec <= get_rule(tokens.curr().type)->prec) {
        node = get_rule(tokens.curr().type)->midfix(tokens, node);
    }
    return node;
}

static const auto rules = std::unordered_map<token_type, parse_rule>
{
    {token_type::left_paren,          {parse_grouping,      parse_call,      precedence::call}},
    {token_type::bang,                {parse_unary,         parse_templates, precedence::call}},
    {token_type::minus,               {parse_unary,         parse_binary,    precedence::term}},
    {token_type::plus,                {nullptr,             parse_binary,    precedence::term}},
    {token_type::slash,               {nullptr,             parse_binary,    precedence::factor}},
    {token_type::star,                {nullptr,             parse_binary,    precedence::factor}},
    {token_type::percent,             {nullptr,             parse_binary,    precedence::factor}},
    {token_type::int32,               {parse_i32,           nullptr,         precedence::none}},
    {token_type::int64,               {parse_i64,           nullptr,         precedence::none}},
    {token_type::uint64,              {parse_u64,           nullptr,         precedence::none}},
    {token_type::float64,             {parse_f64,           nullptr,         precedence::none}},
    {token_type::character,           {parse_char,          nullptr,         precedence::none}},
    {token_type::kw_true,             {parse_true,          nullptr,         precedence::none}},
    {token_type::kw_false,            {parse_false,         nullptr,         precedence::none}},
    {token_type::kw_null,             {parse_null,          nullptr,         precedence::none}},
    {token_type::string,              {parse_string,        nullptr,         precedence::none}},
    {token_type::equal_equal,         {nullptr,             parse_binary,    precedence::equality}},
    {token_type::bang_equal,          {nullptr,             parse_binary,    precedence::equality}},
    {token_type::less,                {nullptr,             parse_binary,    precedence::comparison}},
    {token_type::less_equal,          {nullptr,             parse_binary,    precedence::comparison}},
    {token_type::greater,             {nullptr,             parse_binary,    precedence::comparison}},
    {token_type::greater_equal,       {nullptr,             parse_binary,    precedence::comparison}},
    {token_type::ampersand_ampersand, {nullptr,             parse_binary,    precedence::logical_and}},
    {token_type::bar_bar,             {nullptr,             parse_binary,    precedence::logical_or}},
    {token_type::identifier,          {parse_name,          nullptr,         precedence::none}},
    {token_type::kw_module,           {parse_name,          nullptr,         precedence::none}},
    {token_type::kw_type,             {parse_name,          nullptr,         precedence::none}},
    {token_type::kw_i32,              {parse_name,          nullptr,         precedence::none}},
    {token_type::kw_i64,              {parse_name,          nullptr,         precedence::none}},
    {token_type::kw_f64,              {parse_name,          nullptr,         precedence::none}},
    {token_type::kw_u64,              {parse_name,          nullptr,         precedence::none}},
    {token_type::kw_char,             {parse_name,          nullptr,         precedence::none}},
    {token_type::kw_bool,             {parse_name,          nullptr,         precedence::none}},
    {token_type::kw_null,             {parse_name,          nullptr,         precedence::none}},
    {token_type::kw_arena,            {parse_name,          nullptr,         precedence::none}},
    {token_type::left_bracket,        {parse_array,         parse_subscript, precedence::call}},
    {token_type::dot,                 {nullptr,             parse_dot,       precedence::call}},
    {token_type::kw_const,            {parse_const_pre,     parse_const,     precedence::call}},
    {token_type::at,                  {parse_intrinsic,     parse_at,        precedence::call}},
    {token_type::ampersand,           {parse_ampersand_pre, parse_ampersand, precedence::call}},
    {token_type::kw_function,         {parse_func_ptr,      nullptr,         precedence::none}},
    {token_type::kw_new,              {parse_new,           nullptr,         precedence::none}},
    {token_type::question,            {nullptr,             parse_question,  precedence::ternary}},
    {token_type::kw_as,               {nullptr,             parse_as,        precedence::as}}
};

auto get_rule(token_type tt) -> const parse_rule*
{
    static constexpr auto default_rule = parse_rule{nullptr, nullptr, precedence::none};
    if (rules.contains(tt)) return &rules.at(tt);
    return &default_rule;
}

auto parse_expression(tokenstream& tokens) -> node_expr_ptr
{
    return parse_precedence(tokens, precedence::as);
}

auto parse_identifier(tokenstream& tokens) -> std::string
{
    return std::string{tokens.consume_only(token_type::identifier).text};
}

auto parse_function_def_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_function_stmt>();
    stmt.token = tokens.consume_only(token_type::kw_function);
    stmt.name = parse_identifier(tokens);

    if (tokens.consume_maybe(token_type::bang)) {
        tokens.consume_only(token_type::left_paren);
        tokens.consume_comma_separated_list(token_type::right_paren, [&]{
            stmt.templates.push_back(parse_identifier(tokens));
        });
    }

    tokens.consume_only(token_type::left_paren);
    tokens.consume_comma_separated_list(token_type::right_paren, [&]{
        auto param = node_parameter{};
        param.name = parse_identifier(tokens);
        tokens.consume_only(token_type::colon);
        param.type = parse_expression(tokens);
        stmt.params.push_back(param);
    });

    if (tokens.consume_maybe(token_type::arrow)) {
        stmt.return_type = parse_expression(tokens);
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
    stmt.names = parse_name_pack(tokens);
    stmt.is_ptr = tokens.consume_maybe(token_type::ampersand);
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
    stmt.name = parse_identifier(tokens);
    if (tokens.consume_maybe(token_type::bang)) {
        tokens.consume_only(token_type::left_paren);
        tokens.consume_comma_separated_list(token_type::right_paren, [&] {
            stmt.templates.push_back(parse_identifier(tokens));
        });
    }
    tokens.consume_only(token_type::left_brace);
    while (!tokens.consume_maybe(token_type::right_brace)) {
        if (tokens.peek(token_type::kw_function)) {
            stmt.functions.emplace_back(parse_function_def_stmt(tokens));
        } else {
            auto& f = stmt.fields.emplace_back();
            f.name = parse_identifier(tokens);
            tokens.consume_only(token_type::colon);
            f.type = parse_expression(tokens);
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

    stmt.names = parse_name_pack(tokens);

    if (tokens.consume_maybe(token_type::colon)) {
        stmt.explicit_type = parse_expression(tokens);
        tokens.consume_only(token_type::equal);
    } else {
        tokens.consume_only(token_type::colon_equal);
    }
    stmt.expr = parse_expression(tokens);
    tokens.consume_only(token_type::semicolon);
    return node;
}

auto parse_arena_declaration_stmt(tokenstream& tokens) -> node_stmt_ptr
{
    auto node = std::make_shared<node_stmt>();
    auto& stmt = node->emplace<node_arena_declaration_stmt>();
    stmt.token = tokens.consume();
    stmt.name = parse_identifier(tokens);
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
        case token_type::kw_assert:   return parse_assert_stmt(tokens);
        case token_type::kw_break:    return parse_break_stmt(tokens);
        case token_type::kw_continue: return parse_continue_stmt(tokens);
        case token_type::left_brace:  return parse_braced_statement_list(tokens);
        case token_type::kw_let:
        case token_type::kw_var:      return parse_declaration_stmt(tokens);
        case token_type::kw_arena:    return parse_arena_declaration_stmt(tokens);
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
        seq.sequence.push_back(parse_top_level_statement(stream));
    }
    return new_module;
}

}
