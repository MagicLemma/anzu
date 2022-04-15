#include "runtime.hpp"
#include "object.hpp"
#include "utility/print.hpp"
#include "utility/overloaded.hpp"
#include "utility/scope_timer.hpp"
#include "utility/memory.hpp"

#include <chrono>
#include <utility>

namespace anzu {

template <typename ...Args>
auto runtime_assert(bool condition, std::string_view msg, Args&&... args)
{
    if (!condition) {
        anzu::print(msg, std::forward<Args>(args)...);
        std::exit(1);
    }
}

auto apply_op(runtime_context& ctx, const op& op_code) -> void
{
    std::visit(overloaded {
        [&](const op_load_bytes& op) {
            for (const auto byte : op.bytes) {
                ctx.memory.push_back(byte);
            }
            ++ctx.prog_ptr;
        },
        [&](const op_push_global_addr& op) {
            push_value(ctx.memory, op.position);
            push_value(ctx.memory, op.size);
            ++ctx.prog_ptr;
        },
        [&](const op_push_local_addr& op) {
            push_value(ctx.memory, ctx.base_ptr + op.offset);
            push_value(ctx.memory, op.size);
            ++ctx.prog_ptr;
        },
        [&](op_modify_ptr) {
            const auto new_size = pop_value<std::uint64_t>(ctx.memory);
            const auto offset = pop_value<std::uint64_t>(ctx.memory);
            pop_value<std::uint64_t>(ctx.memory); // Old size
            const auto ptr = pop_value<std::uint64_t>(ctx.memory);

            push_value(ctx.memory, ptr + offset);
            push_value(ctx.memory, new_size);
            ++ctx.prog_ptr;
        },
        [&](op_load) {
            const auto size = pop_value<std::uint64_t>(ctx.memory);
            const auto ptr = pop_value<std::uint64_t>(ctx.memory);
            for (std::size_t i = 0; i != size; ++i) {
                ctx.memory.push_back(ctx.memory[ptr + i]);
            }
            ++ctx.prog_ptr;
        },
        [&](op_save) {
            const auto size = pop_value<std::uint64_t>(ctx.memory);
            const auto ptr = pop_value<std::uint64_t>(ctx.memory);
            runtime_assert(ptr + size <= ctx.memory.size(), "tried to access invalid memory address {}", ptr);
            if (ptr + size < ctx.memory.size()) {
                for (const auto i : std::views::iota(ptr, ptr + size) | std::views::reverse) {
                    ctx.memory[i] = ctx.memory.back();
                    ctx.memory.pop_back();
                }
            }
            ++ctx.prog_ptr;
        },
        [&](const op_pop& op) {
            for (std::size_t i = 0; i != op.size; ++i) {
                ctx.memory.pop_back();
            }
            ++ctx.prog_ptr;
        },
        [&](op_if) {
            ++ctx.prog_ptr;
        },
        [&](op_if_end) {
            ++ctx.prog_ptr;
        },
        [&](const op_else& op) {
            ctx.prog_ptr = op.jump;
        },
        [&](op_loop_begin) {
            ++ctx.prog_ptr;
        },
        [&](const op_loop_end& op) {
            ctx.prog_ptr = op.jump;
        },
        [&](const op_break& op) {
            ctx.prog_ptr = op.jump;
        },
        [&](const op_continue& op) {
            ctx.prog_ptr = op.jump;
        },
        [&](const op_jump_if_false& op) {
            if (ctx.memory.back() == std::byte{1}) {
                ++ctx.prog_ptr;
            } else {
                ctx.prog_ptr = op.jump;
            }
            ctx.memory.pop_back();
        },
        [&](const op_function& op) {
            ctx.prog_ptr = op.jump;
        },
        [&](op_return) {
            const auto prev_base_ptr = read_value<std::uint64_t>(ctx.memory, ctx.base_ptr);
            const auto prev_prog_ptr = read_value<std::uint64_t>(ctx.memory, ctx.base_ptr + sizeof(std::uint64_t));
            const auto return_size = read_value<std::uint64_t>(ctx.memory, ctx.base_ptr + 2*sizeof(std::uint64_t));
            
            for (std::size_t i = 0; i != return_size; ++i) {
                ctx.memory[ctx.base_ptr + i] = ctx.memory[ctx.memory.size() - return_size + i];
            }
            while (ctx.memory.size() > ctx.base_ptr + return_size) {
                ctx.memory.pop_back();
            }
            ctx.base_ptr = prev_base_ptr;
            ctx.prog_ptr = prev_prog_ptr;
        },
        [&](const op_function_call& op) {
            // Store the old base_ptr and prog_ptr so that they can be restored at the end of
            // the function. Note that the return size is stored at new_base_ptr + 2 but and has
            // already been written in.
            const auto new_base_ptr = ctx.memory.size() - op.args_size;
            write_value(ctx.memory, new_base_ptr, ctx.base_ptr);
            write_value(ctx.memory, new_base_ptr + sizeof(std::uint64_t), ctx.prog_ptr + 1); // Pos after function call
            
            ctx.base_ptr = new_base_ptr;
            ctx.prog_ptr = op.ptr; // Jump into the function
        },
        [&](const op_builtin_call& op) {
            op.ptr(ctx.memory);
            ++ctx.prog_ptr;
        },
        [&](const op_builtin_mem_op& op) {
            op.ptr(ctx.memory);
            ++ctx.prog_ptr;
        }
    }, op_code);
}

auto run_program(const anzu::program& program) -> void
{
    const auto timer = scope_timer{};

    runtime_context ctx;
    while (ctx.prog_ptr < program.size()) {
        apply_op(ctx, program[ctx.prog_ptr]);
    }
}

auto run_program_debug(const anzu::program& program) -> void
{
    const auto timer = scope_timer{};

    runtime_context ctx;
    while (ctx.prog_ptr < program.size()) {
        const auto& op = program[ctx.prog_ptr];
        anzu::print("{:>4} - {}\n", ctx.prog_ptr, op);
        apply_op(ctx, program[ctx.prog_ptr]);
        anzu::print("Memory: {}\n", format_comma_separated(ctx.memory));
    }
}

}