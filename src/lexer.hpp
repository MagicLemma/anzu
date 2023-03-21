#pragma once
#include <vector>
#include <filesystem>
#include <format>
#include <cstdint>
#include <string>
#include <memory>
#include <string_view>

#include "token.hpp"

namespace anzu {

struct lex_context
{
    std::string_view::const_iterator start, curr, end;
    std::size_t line = 1;
    std::size_t col = 1;
};

auto read_file(const std::filesystem::path& file) -> std::unique_ptr<std::string>;
auto lex_start(std::string_view source_code) -> lex_context;
auto lex_next(lex_context& ctx) -> token;

class tokenstream
{
    lex_context d_ctx;
    token d_curr;
    token d_next;

public:
    tokenstream(std::string_view source_code);

    auto valid() const -> bool { return d_curr.type != token_type::eof; }
    auto has_next() const -> bool { return d_next.type != token_type::eof; }

    auto curr() const -> const token& { return d_curr; }
    auto next() const -> const token& { return d_next; }
    auto position() const -> std::int64_t { return 0; }

    auto consume() -> token
    {
        return std::exchange(d_curr, std::exchange(d_next, lex_next(d_ctx)));
    }

    auto consume_maybe(token_type tt) -> bool;
    auto consume_only(token_type tt, std::source_location loc = std::source_location::current()) -> token;
    auto consume_i64() -> std::int64_t;
    auto consume_u64() -> std::uint64_t;
    auto peek(token_type tt) -> bool;
    auto peek_next(token_type tt) -> bool;

    template <typename Func>
    auto consume_comma_separated_list(token_type tt, Func&& callback) -> void
    {
        if (consume_maybe(tt)) { // Empty list
            return;
        }
        callback(); // Parse first
        while (!peek(tt)) {
            consume_only(token_type::comma);
            callback();
        }
        consume_only(tt);
    }
};

}