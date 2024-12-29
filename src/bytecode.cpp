#include "bytecode.hpp"

#include <print>
#include <cstddef>
#include <cstring>

namespace anzu {
namespace {

template <typename T>
requires std::integral<T> || std::floating_point<T> || std::is_same_v<T, std::byte*> || std::is_same_v<T, op>
auto read_at(const std::byte** ptr) -> T
{
    auto ret = T{};
    std::memcpy(&ret, *ptr, sizeof(T));
    *ptr += sizeof(T);
    return ret;
}

}

auto print_op(std::string_view rom, const std::byte* start, const std::byte* ptr) -> const std::byte*
{
    std::print("    [{:>3}] ", static_cast<std::size_t>(ptr - start));
    const auto op_code = read_at<op>(&ptr);
    switch (op_code) {
        case op::end_program: {
            std::print("END_PROGRAM\n");
        } break;
        case op::push_i32: {
            const auto value = read_at<std::int32_t>(&ptr);
            std::print("PUSH_I32: {}\n", value);
        } break;
        case op::push_i64: {
            const auto value = read_at<std::int64_t>(&ptr);
            std::print("PUSH_I64: {}\n", value);
        } break;
        case op::push_u64: {
            const auto value = read_at<std::uint64_t>(&ptr);
            std::print("PUSH_U64: {}\n", value);
        } break;
        case op::push_f64: {
            const auto value = read_at<double>(&ptr);
            std::print("PUSH_F64: {}\n", value);
        } break;
        case op::push_char: {
            const auto value = read_at<char>(&ptr);
            std::print("PUSH_CHAR: {}\n", value);
        } break;
        case op::push_bool: {
            const auto value = read_at<bool>(&ptr);
            std::print("PUSH_BOOL: {}\n", value);
        } break;
        case op::push_null: {
            std::print("PUSH_NULL\n");
        } break;
        case op::push_string_literal: {
            const auto index = read_at<std::uint64_t>(&ptr);
            const auto size = read_at<std::uint64_t>(&ptr);
            const auto data = &rom[index];
            const auto m = std::string_view(data, size);
            std::print("PUSH_STRING_LITERAL: '{}'\n", m);
        } break;
        case op::push_ptr_global: {
            const auto offset = read_at<std::uint64_t>(&ptr);
            std::print("PUSH_PTR_GLOBAL: {}\n", offset);
        } break;
        case op::push_ptr_local: {
            const auto offset = read_at<std::uint64_t>(&ptr);
            std::print("PUSH_PTR_LOCAL: base_ptr + {}\n", offset);
        } break;
        case op::push_val_global: {
            const auto offset = read_at<std::uint64_t>(&ptr);
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("PUSH_VAL_GLOBAL: {}, size={}\n", offset, size);
        } break;
        case op::push_val_local: {
            const auto offset = read_at<std::uint64_t>(&ptr);
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("PUSH_VAL_LOCAL: base_ptr + {}, size={}\n", offset, size);
        } break;
        case op::push_function_ptr: {
            const auto id = read_at<std::uint64_t>(&ptr);
            std::print("PUSH_FUNCTION_PTR: id={}\n", id);
        } break;
        case op::nth_element_ptr: {
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("NTH_ELEMENT_PTR: size={}\n", size);
        } break;
        case op::nth_element_val: {
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("NTH_ELEMENT_VAL: size={}\n", size);
        } break;
        case op::span_ptr_to_len: {
            std::print("SPAN_PTR_TO_LEN\n");
        } break;
        case op::push_subspan: {
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("PUSH_SUBSPAN: size={}\n", size);
        } break;
        case op::arena_new: {
            std::print("ARENA_NEW\n");
        } break;
        case op::arena_delete: {
            std::print("ARENA_DELETE\n");
        } break;
        case op::arena_alloc: {
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("ARENA_ALLOC: size={}\n", size);
        } break;
        case op::arena_alloc_array: {
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("ARENA_ALLOC_ARRAY: size={}\n", size);
        } break;
        case op::arena_realloc_array: {
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("ARENA_REALLOC_ARRAY: size={}\n", size);
        } break;
        case op::arena_size: {
            std::print("ARENA_SIZE\n");
        } break;
        case op::load: {
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("LOAD: {}\n", size);
        } break;
        case op::save: {
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("SAVE: {}\n", size);
        } break;
        case op::push: {
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("PUSH: {}\n", size);
        } break;
        case op::pop: {
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("POP: {}\n", size);
        } break;
        case op::memcpy: {
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("MEMCPY: {}\n", size);
        } break;
        case op::memcmp: {
            const auto size = read_at<std::uint64_t>(&ptr);
            std::print("MEMCMP: {}\n", size);
        } break;
        case op::jump: {
            const auto jump = read_at<std::uint64_t>(&ptr);
            std::print("JUMP: jump={}\n", jump);
        } break;
        case op::jump_if_true: {
            const auto jump = read_at<std::uint64_t>(&ptr);
            std::print("JUMP_IF_TRUE: jump={}\n", jump);
        } break;
        case op::jump_if_false: {
            const auto jump = read_at<std::uint64_t>(&ptr);
            std::print("JUMP_IF_FALSE: jump={}\n", jump);
        } break;
        case op::ret: {
            const auto type_size = read_at<std::uint64_t>(&ptr);
            std::print("RETURN: type_size={}\n", type_size);
        } break;
        case op::call_static: {
            const auto id = read_at<std::uint64_t>(&ptr);
            const auto args_size = read_at<std::uint64_t>(&ptr);
            std::print("CALL_PTR: id={} args_size={}\n", id, args_size);
        } break;
        case op::call_ptr: {
            const auto args_size = read_at<std::uint64_t>(&ptr);
            std::print("CALL_PTR: args_size={}\n", args_size);
        } break;
        case op::assert: {
            const auto index = read_at<std::uint64_t>(&ptr);
            const auto size = read_at<std::uint64_t>(&ptr);
            const auto data = &rom[index];
            std::print("ASSERT: msg={}\n", std::string_view{data, size});
        } break;

        case op::read_file: {
            std::print("READ_FILE\n");
        } break;
        
        case op::null_to_i64: { std::print("NULL_TO_I64\n"); } break;
        case op::bool_to_i64: { std::print("BOOL_TO_I64\n"); } break;
        case op::char_to_i64: { std::print("CHAR_TO_I64\n"); } break;
        case op::i32_to_i64: { std::print("I32_TO_I64\n"); } break;
        case op::u64_to_i64: { std::print("U64_TO_I64\n"); } break;
        case op::f64_to_i64: { std::print("F64_TO_I64\n"); } break;

        case op::null_to_u64: { std::print("NULL_TO_U64\n"); } break;
        case op::bool_to_u64: { std::print("BOOL_TO_U64\n"); } break;
        case op::char_to_u64: { std::print("CHAR_TO_U64\n"); } break;
        case op::i32_to_u64: { std::print("I32_TO_U64\n"); } break;
        case op::i64_to_u64: { std::print("I64_TO_U64\n"); } break;
        case op::f64_to_u64: { std::print("F64_TO_U64\n"); } break;
        
        case op::char_eq: { std::print("CHAR_EQ\n"); } break;
        case op::char_ne: { std::print("CHAR_NE\n"); } break;
        case op::i32_add: { std::print("I32_ADD\n"); } break;
        case op::i32_sub: { std::print("I32_SUB\n"); } break;
        case op::i32_mul: { std::print("I32_MUL\n"); } break;
        case op::i32_div: { std::print("I32_DIV\n"); } break;
        case op::i32_mod: { std::print("I32_MOD\n"); } break;
        case op::i32_eq:  { std::print("I32_EQ\n"); } break;
        case op::i32_ne:  { std::print("I32_NE\n"); } break;
        case op::i32_lt:  { std::print("I32_LT\n"); } break;
        case op::i32_le:  { std::print("I32_LE\n"); } break;
        case op::i32_gt:  { std::print("I32_GT\n"); } break;
        case op::i32_ge:  { std::print("I32_GE\n"); } break;
        case op::i64_add: { std::print("I64_ADD\n"); } break;
        case op::i64_sub: { std::print("I64_SUB\n"); } break;
        case op::i64_mul: { std::print("I64_MUL\n"); } break;
        case op::i64_div: { std::print("I64_DIV\n"); } break;
        case op::i64_mod: { std::print("I64_MOD\n"); } break;
        case op::i64_eq:  { std::print("I64_EQ\n"); } break;
        case op::i64_ne:  { std::print("I64_NE\n"); } break;
        case op::i64_lt:  { std::print("I64_LT\n"); } break;
        case op::i64_le:  { std::print("I64_LE\n"); } break;
        case op::i64_gt:  { std::print("I64_GT\n"); } break;
        case op::i64_ge:  { std::print("I64_GE\n"); } break;
        case op::u64_add: { std::print("U64_ADD\n"); } break;
        case op::u64_sub: { std::print("U64_SUB\n"); } break;
        case op::u64_mul: { std::print("U64_MUL\n"); } break;
        case op::u64_div: { std::print("U64_DIV\n"); } break;
        case op::u64_mod: { std::print("U64_MOD\n"); } break;
        case op::u64_eq:  { std::print("U64_EQ\n"); } break;
        case op::u64_ne:  { std::print("U64_NE\n"); } break;
        case op::u64_lt:  { std::print("U64_LT\n"); } break;
        case op::u64_le:  { std::print("U64_LE\n"); } break;
        case op::u64_gt:  { std::print("U64_GT\n"); } break;
        case op::u64_ge:  { std::print("U64_GE\n"); } break;
        case op::f64_add: { std::print("F64_ADD\n"); } break;
        case op::f64_sub: { std::print("F64_SUB\n"); } break;
        case op::f64_mul: { std::print("F64_MUL\n"); } break;
        case op::f64_div: { std::print("F64_DIV\n"); } break;
        case op::f64_eq:  { std::print("F64_EQ\n"); } break;
        case op::f64_ne:  { std::print("F64_NE\n"); } break;
        case op::f64_lt:  { std::print("F64_LT\n"); } break;
        case op::f64_le:  { std::print("F64_LE\n"); } break;
        case op::f64_gt:  { std::print("F64_GT\n"); } break;
        case op::f64_ge:  { std::print("F64_GE\n"); } break;
        case op::bool_eq:  { std::print("BOOL_EQ\n"); } break;
        case op::bool_ne:  { std::print("BOOL_NE\n"); } break;
        case op::bool_not: { std::print("BOOL_NOT\n"); } break;
        case op::i32_neg: { std::print("I32_NEG\n"); } break;
        case op::i64_neg: { std::print("I64_NEG\n"); } break;
        case op::f64_neg: { std::print("F64_NEG\n"); } break;
        case op::print_null: { std::print("PRINT_NULL\n"); } break;
        case op::print_bool: { std::print("PRINT_BOOL\n"); } break;
        case op::print_char: { std::print("PRINT_CHAR\n"); } break;
        case op::print_i32: { std::print("PRINT_I32\n"); } break;
        case op::print_i64: { std::print("PRINT_I64\n"); } break;
        case op::print_u64: { std::print("PRINT_U64\n"); } break;
        case op::print_f64: { std::print("PRINT_F64\n"); } break;
        case op::print_char_span: { std::print("PRINT_STRING_LITERAL\n"); break;
        case op::print_ptr: { std::print("PRINT_PTR\n"); } break;
        } break;
        default: {
            std::print("UNKNOWN\n");
            return 0;
        } break;
    }
    return ptr;
}

auto linebreak() { std::print("==================================\n"); }

auto print_program(const bytecode_program& prog) -> void
{
    std::print("PROGRAM (num functions = {})\n", prog.functions.size());
    linebreak();
    for (const auto& func : prog.functions) {
        std::print("{} - id: {}\n", func.name, func.id);
        linebreak();
        auto ptr = func.code.data();
        while (ptr < func.code.data() + func.code.size()) {
            ptr = print_op(prog.rom, func.code.data(), ptr);
        }
        std::print("\n");
        linebreak();
    }
    std::print("ROM\n");
    linebreak();
    std::print("{}\n", prog.rom);
    linebreak();
}

}
