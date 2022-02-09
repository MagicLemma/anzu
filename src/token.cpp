#include "token.hpp"
#include "utility/print.hpp"

namespace anzu {

auto to_string(token_type type) -> std::string
{
    switch (type) {
        break; case token_type::keyword: { return "keyword"; };
        break; case token_type::symbol:  { return "symbol"; };
        break; case token_type::name:    { return "name"; };
        break; case token_type::number:  { return "number"; };
        break; case token_type::string:  { return "string"; };
        break; default:                  { return "UNKNOWN"; };
    }
}

auto print_tokens(const std::vector<anzu::token>& tokens) -> void
{
    for (const auto& token : tokens) {
        const auto text = std::format("'{}'", token.text);
        anzu::print(
            "{:<10} - {:<20} {:<5} {:<5}\n",
            anzu::to_string(token.type), text, token.line, token.col
        );
    }
}

tokenstream::tokenstream(const std::vector<token>& tokens)
    : peekstream{tokens}
{}

auto tokenstream::consume_maybe(std::string_view text) -> bool
{
    if (curr().text == text) {
        consume();
        return true;
    }
    return false;
}

auto tokenstream::consume_only(std::string_view text) -> token
{
    if (!valid()) {
        anzu::print("[ERROR] (EOF) expected '{}'\n", text);
        std::exit(1);
    }
    if (!peek(text)) {
        const auto& [tok_text, line, col, type] = curr();
        anzu::print("[ERROR] ({}:{}}) expected '{}', got '{}\n", line, col, text, tok_text);
        std::exit(1);
    }
    return consume();
}

auto tokenstream::consume_only(token_type type) -> token
{
    if (!valid()) {
        anzu::print("[ERROR] (EOF) expected a type\n");
        std::exit(1);
    }
    if (curr().type != type) {
        const auto& [tok_text, line, col, type] = curr();
        anzu::print("[ERROR] ({}:{}}) expected  a type, got '{}\n", line, col, tok_text);
        std::exit(1);
    }
    return consume();
}

auto tokenstream::peek(std::string_view text) -> bool
{
    return valid() && curr().text == text;
}

auto tokenstream::peek_next(std::string_view text) -> bool
{
    return has_next() && next().text == text;
}

}