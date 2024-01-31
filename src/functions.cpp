#include "functions.hpp"
#include "object.hpp"
#include "bytecode.hpp"
#include "utility/common.hpp"
#include "utility/memory.hpp"

#include <unordered_map>
#include <string>
#include <functional>
#include <utility>

namespace anzu {
namespace {

auto resolve_ptr(bytecode_context& ctx, std::uint64_t ptr) -> std::byte*
{
    if (is_heap_ptr(ptr)) {
        const auto index = unset_heap_bit(ptr);
        return &ctx.heap[index];
    }
    if (is_rom_ptr(ptr)) {
        const auto index = unset_rom_bit(ptr);
        return &ctx.rom[index];
    }
    const auto index = ptr;
    return &ctx.stack[index];
}

auto pop_char_span(bytecode_context& ctx) -> std::string
{
    const auto size = pop_value<std::uint64_t>(ctx.stack);
    const auto ptr = pop_value<std::uint64_t>(ctx.stack);
    const auto real_ptr = resolve_ptr(ctx, ptr);
    
    auto ret = std::string(size, ' ');
    std::memcpy(ret.data(), real_ptr, size);
    return ret;
}

auto builtin_sqrt(bytecode_context& ctx) -> void
{
    auto val = pop_value<double>(ctx.stack);
    push_value(ctx.stack, std::sqrt(val));
}

static_assert(sizeof(std::FILE*) == sizeof(std::uint64_t));

auto builtin_fopen(bytecode_context& ctx) -> void
{
    const auto mode = pop_char_span(ctx);
    const auto file = pop_char_span(ctx);
    const auto handle = std::fopen(file.c_str(), mode.c_str());
    push_value(ctx.stack, handle);
}

auto builtin_fclose(bytecode_context& ctx) -> void
{
    const auto ptr = pop_value<std::FILE*>(ctx.stack);
    std::fclose(ptr);
    ctx.stack.push_back(std::byte{0}); // returns null
}

auto builtin_fputs(bytecode_context& ctx) -> void
{
    const auto data = pop_char_span(ctx);
    const auto ptr = pop_value<std::FILE*>(ctx.stack);
    std::fputs(data.c_str(), ptr);
    ctx.stack.push_back(std::byte{0}); // returns null
}

}

auto construct_builtin_array() -> std::vector<builtin>
{
    const auto char_span = char_type().add_const().add_span().add_const();

    auto b = std::vector<builtin>{};

    b.push_back(builtin{"sqrt", builtin_sqrt, {f64_type()}, f64_type()});

    b.push_back(builtin{"fopen", builtin_fopen, {char_span, char_span}, u64_type()});
    b.push_back(builtin{"fclose", builtin_fclose, {u64_type()}, null_type()});
    b.push_back(builtin{"fputs", builtin_fputs, {u64_type(), char_span}, null_type()});

    return b;
}

static const auto builtins = construct_builtin_array();

auto get_builtins() -> std::span<const builtin>
{
    return builtins;
}

auto get_builtin(std::size_t id) -> const builtin&
{
    return builtins[id];
}

}