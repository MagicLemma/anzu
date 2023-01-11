#pragma once
#include "vocabulary.hpp"
#include "utility/peekstream.hpp"

#include <string>
#include <format>
#include <vector>

namespace anzu {

enum class token_type
{
    keyword,
    symbol,
    name,
    character,
    i32,
    i64,
    u64,
    f64,
    string
};

struct token
{
    std::string  text;
    std::int64_t line;
    std::int64_t col;
    token_type   type;

    [[noreturn]] void error(std::string_view msg) const;

    template <typename... Args>
    [[noreturn]] void error(std::string_view msg, Args&&... args) const
    {
        error(std::format(msg, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void assert(bool condition, std::string_view msg, Args&&... args) const
    {
        if (!condition) error(std::format(msg, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void assert_eq(const auto& lhs, const auto& rhs, std::string_view msg, Args&&... args) const
    {
        if (lhs != rhs) {
            const auto user_msg = std::format(msg, std::forward<Args>(args)...);
            error("{}: expected {}, got {}", user_msg, rhs, lhs);
        }
    }
};

auto to_string(token_type type) -> std::string;
auto print_tokens(const std::vector<anzu::token>& tokens) -> void;

class tokenstream : public anzu::peekstream<std::vector<token>>
{
public:
    tokenstream(const std::vector<token>& tokens);
    auto consume_maybe(std::string_view text) -> bool;
    auto consume_only(std::string_view text) -> token;
    auto consume_i64() -> std::int64_t;
    auto consume_u64() -> std::uint64_t;

    template <typename Func>
    auto consume_comma_separated_list(std::string_view sentinel, Func&& callback) -> void
    {
        if (consume_maybe(sentinel)) { // Empty list
            return;
        }
        callback(); // Parse first
        while (!peek(sentinel)) {
            consume_only(tk_comma);
            callback();
        }
        consume_only(sentinel);
    }

    // TODO: Rename these and the peekstream functions to be more consistent
    auto peek(std::string_view text) -> bool;
    auto peek_next(std::string_view text) -> bool;
};
    
}

template <> struct std::formatter<anzu::token_type> : std::formatter<std::string> {
    auto format(const anzu::token_type& tt, auto& ctx) {
        return std::formatter<std::string>::format(to_string(tt), ctx);
    }
};