#include "runtime.hpp"
#include "bytecode.hpp"
#include "object.hpp"

#include <functional>
#include <utility>
#include <format>

namespace anzu {
namespace {

template <typename ...Args>
[[noreturn]] auto runtime_error(std::format_string<Args...> message, Args&&... args)
{
    const auto msg = std::format(message, std::forward<Args>(args)...);
    panic("runtime assertion failed! {}", msg);
}

template <typename Type, template <typename> typename Op>
auto unary_op(bytecode_context& ctx) -> void
{
    static constexpr auto op = Op<Type>{};
    const auto obj = ctx.stack.pop<Type>();
    ctx.stack.push(op(obj));
}

template <typename Type, template <typename> typename Op>
auto binary_op(bytecode_context& ctx) -> void
{
    static constexpr auto op = Op<Type>{};
    const auto rhs = ctx.stack.pop<Type>();
    const auto lhs = ctx.stack.pop<Type>();
    ctx.stack.push(op(lhs, rhs));
}

template <typename Type>
auto print_value(bytecode_context& ctx) -> void
{
    const auto obj = ctx.stack.pop<Type>();
    std::print("{}", obj);
}

template <typename T>
requires std::integral<T> || std::floating_point<T> || std::same_as<T, std::byte*> || std::same_as<T, op>
auto read_advance(bytecode_context& ctx) -> T
{
    T ret;
    std::memcpy(&ret, ctx.frames.back().ip, sizeof(T));
    ctx.frames.back().ip += sizeof(T);
    return ret;
}

template <bool Debug>
auto execute_program(bytecode_context& ctx) -> void
{
    while (true) {
        auto& frame = ctx.frames.back();
        if constexpr (Debug) {
            print_op(ctx.rom, frame.code, frame.ip);
        }
        const auto op_code = read_advance<op>(ctx);
        switch (op_code) {
            case op::end_program: return;
            case op::push_char:
            case op::push_bool: {
                ctx.stack.push(read_advance<std::uint8_t>(ctx));
            } break;
            case op::push_i32: {
                ctx.stack.push(read_advance<std::uint32_t>(ctx));
            } break;
            case op::push_i64:
            case op::push_u64:
            case op::push_f64:
            case op::push_function_ptr: {
                ctx.stack.push(read_advance<std::uint64_t>(ctx));
            } break;
            case op::push_string_literal: {
                const auto index = read_advance<std::uint64_t>(ctx);
                const auto size = read_advance<std::uint64_t>(ctx);
                ctx.stack.push(&ctx.rom[index]);
                ctx.stack.push(size);
            } break;
            case op::push_null: {
                ctx.stack.push(std::byte{0});
            } break;
            case op::push_ptr_global: {
                const auto offset = read_advance<std::uint64_t>(ctx);
                std::byte* ptr = &ctx.stack.at(offset);
                ctx.stack.push(ptr);
            } break;
            case op::push_ptr_local: {
                const auto offset = read_advance<std::uint64_t>(ctx);
                std::byte* ptr = &ctx.stack.at(frame.base_ptr + offset);
                ctx.stack.push(ptr);
            } break;
            case op::push_val_global: {
                const auto offset = read_advance<std::uint64_t>(ctx);
                const auto size = read_advance<std::uint64_t>(ctx);
                std::byte* ptr = &ctx.stack.at(offset);
                ctx.stack.push(ptr, size);
            } break;
            case op::push_val_local: {
                const auto offset = read_advance<std::uint64_t>(ctx);
                const auto size = read_advance<std::uint64_t>(ctx);
                std::byte* ptr = &ctx.stack.at(frame.base_ptr + offset);
                ctx.stack.push(ptr, size);
            } break;
            case op::nth_element_ptr: {
                const auto size = read_advance<std::uint64_t>(ctx);
                const auto index = ctx.stack.pop<std::uint64_t>();
                const auto ptr = ctx.stack.pop<std::byte*>();
                ctx.stack.push(ptr + index * size);
            } break;
            case op::nth_element_val: {
                const auto size = read_advance<std::uint64_t>(ctx);
                const auto index = ctx.stack.pop<std::uint64_t>();
                const auto ptr = ctx.stack.pop<std::byte*>();
                ctx.stack.push(ptr + index * size, size);
            } break;
            case op::span_ptr_to_len: {
                const std::byte* ptr = ctx.stack.pop<std::byte*>();
                ctx.stack.push(ptr + sizeof(std::byte*), sizeof(std::uint64_t));
            } break;
            case op::push_subspan: {
                const auto type_size = read_advance<std::uint64_t>(ctx);
                const auto upper = ctx.stack.pop<std::uint64_t>();
                const auto lower = ctx.stack.pop<std::uint64_t>();
                const auto ptr = ctx.stack.pop<std::byte*>();
                ctx.stack.push(ptr + type_size * lower);
                ctx.stack.push(upper - lower);
            } break;
            case op::load: {
                const auto size = read_advance<std::uint64_t>(ctx);
                const auto ptr = ctx.stack.pop<std::byte*>();
                ctx.stack.push(ptr, size);
            } break;
            case op::save: {
                const auto size = read_advance<std::uint64_t>(ctx);
                const auto ptr = ctx.stack.pop<std::byte*>();
                ctx.stack.pop_and_save(ptr, size);
            } break;
            case op::push: {
                const auto size = read_advance<std::uint64_t>(ctx);
                ctx.stack.resize(ctx.stack.size() + size);
            } break;
            case op::pop: {
                const auto size = read_advance<std::uint64_t>(ctx);
                ctx.stack.resize(ctx.stack.size() - size);
            } break;
            case op::memcpy: {
                const auto type_size = read_advance<std::uint64_t>(ctx);
                const auto src_count = ctx.stack.pop<std::uint64_t>(); 
                const auto src_data = ctx.stack.pop<std::byte*>();
                const auto dst_count = ctx.stack.pop<std::uint64_t>(); 
                const auto dst_data = ctx.stack.pop<std::byte*>();
                if (dst_count < src_count) {
                    runtime_error("dst span too small to hold src span");
                }
                std::memcpy(dst_data, src_data, src_count * type_size);
                ctx.stack.push(std::byte{0}); // returns null;
            } break;
            case op::memcmp: {
                const auto type_size = read_advance<std::uint64_t>(ctx); 
                const auto rhs_data = ctx.stack.pop<std::byte*>();
                const auto lhs_data = ctx.stack.pop<std::byte*>();
                const bool equal = std::memcmp(lhs_data, rhs_data, type_size) == 0;
                ctx.stack.push(equal); // returns null;
            } break;
            case op::arena_new: {
                memory_arena* arena = nullptr;
                if (ctx.arena_free_list.empty()) {
                    ctx.arenas.push_back(std::make_unique<memory_arena>());
                    arena = ctx.arenas.back().get();
                    arena->index = ctx.arenas.size() - 1;
                } else {
                    const auto index = ctx.arena_free_list.back();
                    ctx.arena_free_list.pop_back();
                    arena = ctx.arenas.at(index).get();
                }
                arena->next = 0;
                ctx.stack.push(arena);
            } break;
            case op::arena_delete: {
                const auto arena = ctx.stack.pop<memory_arena*>();
                ctx.arena_free_list.push_back(arena->index);
            } break;
            case op::arena_alloc: {
                auto arena = ctx.stack.pop<memory_arena*>();
                const auto size = read_advance<std::uint64_t>(ctx);
                if (arena->next + size > arena->data.size()) {
                    runtime_error("arena overflow");
                }
                const auto data = &arena->data[arena->next];
                arena->next += size;
                ctx.stack.pop_and_save(data, size);
                ctx.stack.push(data);
            } break;
            case op::arena_alloc_array: {
                const auto type_size = read_advance<std::uint64_t>(ctx);
                auto arena = ctx.stack.pop<memory_arena*>();
                const auto count = ctx.stack.pop<std::uint64_t>();
                const auto size = type_size * count;
                if (arena->next + size > arena->data.size()) {
                    runtime_error("arena overflow");
                }
                const auto data = &arena->data[arena->next];
                for (size_t i = 0; i != count; ++i) {
                    ctx.stack.save(data + i * type_size, type_size);
                }
                ctx.stack.pop_n(type_size);
                arena->next += size;
                ctx.stack.push(data); // push the span (ptr + count)
                ctx.stack.push(count);
            } break;
            case op::arena_realloc_array: {
                const auto type_size = read_advance<std::uint64_t>(ctx);
                const auto old_count = ctx.stack.pop<std::uint64_t>(); // this is the 
                const auto old_data = ctx.stack.pop<std::byte*>();     // pushed span
                auto arena = ctx.stack.pop<memory_arena*>();
                const auto new_count = ctx.stack.pop<std::uint64_t>();
                const auto size = type_size * new_count;
                if (new_count <= old_count) {
                    runtime_error("invalid use of new, can only realloc to grow, old={} new={}", old_count, new_count);
                }
                if (arena->next + size > arena->data.size()) {
                    runtime_error("arena overflow");
                }
                const auto new_data = &arena->data[arena->next];
                std::memcpy(new_data, old_data, type_size * old_count);
                for (size_t i = old_count; i != new_count; ++i) {
                    ctx.stack.save(new_data + i * type_size, type_size);
                }
                ctx.stack.pop_n(type_size);
                arena->next += size;
                ctx.stack.push(new_data); // push the span (ptr + count)
                ctx.stack.push(new_count);
            } break;
            case op::arena_size: {
                auto arena = ctx.stack.pop<memory_arena*>();
                ctx.stack.push(arena->next);
            } break;
            case op::jump: {
                const auto jump = read_advance<std::uint64_t>(ctx);
                frame.ip = &frame.code[jump];
            } break;
            case op::jump_if_true: {
                const auto jump = read_advance<std::uint64_t>(ctx);
                if (ctx.stack.pop<bool>()) frame.ip = &frame.code[jump];
            } break;
            case op::jump_if_false: {
                const auto jump = read_advance<std::uint64_t>(ctx);
                if (!ctx.stack.pop<bool>()) frame.ip = &frame.code[jump];
            } break;
            case op::ret: {
                const auto size = read_advance<std::uint64_t>(ctx);
                std::memcpy(&ctx.stack.at(frame.base_ptr), &ctx.stack.at(ctx.stack.size() - size), size);
                ctx.stack.resize(frame.base_ptr + size);
                ctx.frames.pop_back();
            } break;
            case op::call_static: {
                const auto function_id = read_advance<std::uint64_t>(ctx);
                const auto args_size = read_advance<std::uint64_t>(ctx);
                ctx.frames.push_back(call_frame{
                    .code = ctx.functions[function_id].code.data(),
                    .ip = ctx.functions[function_id].code.data(),
                    .base_ptr = ctx.stack.size() - args_size
                });
            } break;
            case op::call_ptr: {
                const auto args_size = read_advance<std::uint64_t>(ctx);
                const auto function_id = ctx.stack.pop<std::uint64_t>();
                ctx.frames.push_back(call_frame{
                    .code = ctx.functions[function_id].code.data(),
                    .ip = ctx.functions[function_id].code.data(),
                    .base_ptr = ctx.stack.size() - args_size
                });
            } break;
            case op::assert: {
                const auto index = read_advance<std::uint64_t>(ctx);
                const auto size = read_advance<std::uint64_t>(ctx);
                if (!ctx.stack.pop<bool>()) {
                    const auto data = &ctx.rom[index];
                    runtime_error("{}", std::string_view{data, size});
                }
            } break;

            case op::read_file: {
                auto arena = ctx.stack.pop<memory_arena*>();
                const auto filename_size = ctx.stack.pop<std::uint64_t>();
                const auto filename_data = ctx.stack.pop<char*>();
                const auto file = std::string{filename_data, filename_size};
                const auto handle = std::fopen(file.c_str(), "rb");
                if (!handle) {
                    std::print("failed to open\n");
                    std::exit(1);
                }
                std::fseek(handle, 0, SEEK_END);
                const auto ssize = std::ftell(handle);
                if (ssize == -1) {
                    std::print("Error with ftell\n");
                    std::exit(1);
                }
                const auto size = static_cast<std::size_t>(ssize);
                std::rewind(handle);
                std::byte* ptr = &arena->data[arena->next];
                const auto bytes_read = std::fread(ptr, sizeof(std::byte), ssize, handle);
                if (bytes_read != ssize) {
                    std::print("Error with fread\n");
                    std::exit(1);
                }	
                arena->next += size;

                std::fclose(handle);
                ctx.stack.push(ptr);  // push the
                ctx.stack.push(size); // span
            } break;

            case op::null_to_i64: {
                const auto value = ctx.stack.pop<std::byte>();
                ctx.stack.push(std::int64_t{0});
            } break;
            case op::bool_to_i64: {
                const auto value = ctx.stack.pop<bool>();
                ctx.stack.push(static_cast<std::int64_t>(value));
            } break;
            case op::char_to_i64: {
                const auto value = ctx.stack.pop<char>();
                ctx.stack.push(static_cast<std::int64_t>(value));
            } break;
            case op::i32_to_i64: {
                const auto value = ctx.stack.pop<std::int32_t>();
                ctx.stack.push(static_cast<std::int64_t>(value));
            } break;
            case op::u64_to_i64: {
                const auto value = ctx.stack.pop<std::uint64_t>();
                ctx.stack.push(static_cast<std::int64_t>(value));
            } break;
            case op::f64_to_i64: {
                const auto value = ctx.stack.pop<double>();
                ctx.stack.push(static_cast<std::int64_t>(value));
            } break;

            case op::null_to_u64: {
                const auto value = ctx.stack.pop<std::byte>();
                ctx.stack.push(std::uint64_t{0});
            } break;
            case op::bool_to_u64: {
                const auto value = ctx.stack.pop<bool>();
                ctx.stack.push(static_cast<std::uint64_t>(value));
            } break;
            case op::char_to_u64: {
                const auto value = ctx.stack.pop<char>();
                ctx.stack.push(static_cast<std::uint64_t>(value));
            } break;
            case op::i32_to_u64: {
                const auto value = ctx.stack.pop<std::int32_t>();
                ctx.stack.push(static_cast<std::uint64_t>(value));
            } break;
            case op::i64_to_u64: {
                const auto value = ctx.stack.pop<std::int64_t>();
                ctx.stack.push(static_cast<std::uint64_t>(value));
            } break;
            case op::f64_to_u64: {
                const auto value = ctx.stack.pop<double>();
                ctx.stack.push(static_cast<std::uint64_t>(value));
            } break;

            case op::char_eq: { binary_op<char, std::equal_to>(ctx); } break;
            case op::char_ne: { binary_op<char, std::not_equal_to>(ctx); } break;

            case op::i32_add: { binary_op<std::int32_t, std::plus>(ctx); } break;
            case op::i32_sub: { binary_op<std::int32_t, std::minus>(ctx); } break;
            case op::i32_mul: { binary_op<std::int32_t, std::multiplies>(ctx); } break;
            case op::i32_div: { binary_op<std::int32_t, std::divides>(ctx); } break;
            case op::i32_mod: { binary_op<std::int32_t, std::modulus>(ctx); } break;
            case op::i32_eq:  { binary_op<std::int32_t, std::equal_to>(ctx); } break;
            case op::i32_ne:  { binary_op<std::int32_t, std::not_equal_to>(ctx); } break;
            case op::i32_lt:  { binary_op<std::int32_t, std::less>(ctx); } break;
            case op::i32_le:  { binary_op<std::int32_t, std::less_equal>(ctx); } break;
            case op::i32_gt:  { binary_op<std::int32_t, std::greater>(ctx); } break;
            case op::i32_ge:  { binary_op<std::int32_t, std::greater_equal>(ctx); } break;

            case op::i64_add: { binary_op<std::int64_t, std::plus>(ctx); } break;
            case op::i64_sub: { binary_op<std::int64_t, std::minus>(ctx); } break;
            case op::i64_mul: { binary_op<std::int64_t, std::multiplies>(ctx); } break;
            case op::i64_div: { binary_op<std::int64_t, std::divides>(ctx); } break;
            case op::i64_mod: { binary_op<std::int64_t, std::modulus>(ctx); } break;
            case op::i64_eq:  { binary_op<std::int64_t, std::equal_to>(ctx); } break;
            case op::i64_ne:  { binary_op<std::int64_t, std::not_equal_to>(ctx); } break;
            case op::i64_lt:  { binary_op<std::int64_t, std::less>(ctx); } break;
            case op::i64_le:  { binary_op<std::int64_t, std::less_equal>(ctx); } break;
            case op::i64_gt:  { binary_op<std::int64_t, std::greater>(ctx); } break;
            case op::i64_ge:  { binary_op<std::int64_t, std::greater_equal>(ctx); } break;

            case op::u64_add: { binary_op<std::uint64_t, std::plus>(ctx); } break;
            case op::u64_sub: { binary_op<std::uint64_t, std::minus>(ctx); } break;
            case op::u64_mul: { binary_op<std::uint64_t, std::multiplies>(ctx); } break;
            case op::u64_div: { binary_op<std::uint64_t, std::divides>(ctx); } break;
            case op::u64_mod: { binary_op<std::uint64_t, std::modulus>(ctx); } break;
            case op::u64_eq:  { binary_op<std::uint64_t, std::equal_to>(ctx); } break;
            case op::u64_ne:  { binary_op<std::uint64_t, std::not_equal_to>(ctx); } break;
            case op::u64_lt:  { binary_op<std::uint64_t, std::less>(ctx); } break;
            case op::u64_le:  { binary_op<std::uint64_t, std::less_equal>(ctx); } break;
            case op::u64_gt:  { binary_op<std::uint64_t, std::greater>(ctx); } break;
            case op::u64_ge:  { binary_op<std::uint64_t, std::greater_equal>(ctx); } break;

            case op::f64_add: { binary_op<double, std::plus>(ctx); } break;
            case op::f64_sub: { binary_op<double, std::minus>(ctx); } break;
            case op::f64_mul: { binary_op<double, std::multiplies>(ctx); } break;
            case op::f64_div: { binary_op<double, std::divides>(ctx); } break;
            case op::f64_eq:  { binary_op<double, std::equal_to>(ctx); } break;
            case op::f64_ne:  { binary_op<double, std::not_equal_to>(ctx); } break;
            case op::f64_lt:  { binary_op<double, std::less>(ctx); } break;
            case op::f64_le:  { binary_op<double, std::less_equal>(ctx); } break;
            case op::f64_gt:  { binary_op<double, std::greater>(ctx); } break;
            case op::f64_ge:  { binary_op<double, std::greater_equal>(ctx); } break;

            case op::bool_eq:  { binary_op<bool, std::equal_to>(ctx); } break;
            case op::bool_ne:  { binary_op<bool, std::not_equal_to>(ctx); } break;
            case op::bool_not: { unary_op<bool, std::logical_not>(ctx); } break;

            case op::i32_neg: { unary_op<std::int32_t, std::negate>(ctx); } break;
            case op::i64_neg: { unary_op<std::int64_t, std::negate>(ctx); } break;
            case op::f64_neg: { unary_op<double, std::negate>(ctx); } break;

            case op::print_null: {
                ctx.stack.pop<std::byte>(); // pops the null byte
                std::print("null");
            } break;
            case op::print_bool: {
                const auto b = ctx.stack.pop<bool>();
                std::print("{}", b ? "true" : "false");
            } break;
            case op::print_char: {
                const auto c = ctx.stack.pop<char>();
                std::print("{}", c);
            } break;
            case op::print_i32: { print_value<std::int32_t>(ctx); } break;
            case op::print_i64: { print_value<std::int64_t>(ctx); } break;
            case op::print_u64: { print_value<std::uint64_t>(ctx); } break;
            case op::print_f64: { print_value<double>(ctx); } break;
            case op::print_char_span: {
                const auto size = ctx.stack.pop<std::uint64_t>();
                const auto ptr = ctx.stack.pop<const char*>();
                std::print("{}", std::string_view{ptr, size});
            } break;
            case op::print_ptr: {
                const auto ptr = ctx.stack.pop<std::uint64_t>();
                std::print("{:#018x}", ptr);
            } break; 

            default: { runtime_error("unknown op code! ({})", static_cast<int>(op_code)); } break;
        }
    }
}

template <bool Debug>
auto run(const bytecode_program& prog) -> void
{
    bytecode_context ctx{prog.functions, prog.rom};
    ctx.frames.reserve(1000);
    ctx.frames.emplace_back(call_frame{
        .code = ctx.functions.front().code.data(),
        .ip = ctx.functions.front().code.data(),
        .base_ptr = 0
    });

    execute_program<Debug>(ctx);

    if (ctx.stack.size() > 0) {
        std::print("\n -> Stack Size: {}, bug in the compiler!\n", ctx.stack.size());
    }
}

}

vm_stack::vm_stack(std::size_t size)
    : d_data{std::make_unique<std::byte[]>(size)}
    , d_max_size{size}
    , d_current_size{0}
{}

auto vm_stack::push(const std::byte* src, std::size_t count) -> void
{
    if (d_current_size + count > d_max_size) {
        std::print("Stack overflow (current_size={}, count={}, max_size={}\n", d_current_size, count, d_max_size);
        std::exit(27);
    }
    std::memcpy(&d_data[d_current_size], src, count);
    d_current_size += count;
}

auto vm_stack::pop_and_save(std::byte* dst, std::size_t count) -> void
{
    save(dst, count);
    d_current_size -= count;
}

auto vm_stack::save(std::byte* dst, std::size_t count) -> void
{
    if (d_current_size < count) {
        std::print("Stack underflow\n");
        std::exit(28);
    }
    std::memcpy(dst, &d_data[d_current_size - count], count);
}

auto vm_stack::size() const -> std::size_t { return d_current_size; }
auto vm_stack::at(std::size_t index) -> std::byte& { return d_data[index]; }
auto vm_stack::resize(std::size_t size) -> void { d_current_size = size; }
auto vm_stack::pop_n(std::size_t size) -> void { d_current_size -= size; }

auto vm_stack::print() const -> void
{
    for (std::size_t idx = 0; idx != 100; ++idx) {
        std::print("{} ", d_data[idx]);
    }
    std::print("\n");
}

auto run_program(const bytecode_program& prog) -> void
{
    run<false>(prog);
}

auto run_program_debug(const bytecode_program& prog) -> void
{
    run<true>(prog);
}

}