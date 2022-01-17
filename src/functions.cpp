#include "functions.hpp"
#include "print.hpp"

#include <unordered_map>
#include <string>
#include <functional>

namespace {

auto verify(bool condition, std::string_view msg) -> void
{
    if (!condition) {
        anzu::print(msg);
        std::exit(1);
    }
}

auto builtin_print(anzu::context& ctx) -> void
{
    verify(!ctx.top().empty(), "frame stack is empty, nothing to print\n");
    anzu::print("{}\n", ctx.top().pop());
}

auto builtin_stack_size(anzu::context& ctx) -> void
{
    auto stack_size = static_cast<int>(ctx.top().stack_size());
    ctx.top().push(stack_size);
};

auto builtin_print_frame(anzu::context& ctx) -> void
{
    auto& frame = ctx.top();
    frame.print();
}

auto builtin_list_new(anzu::context& ctx) -> void
{
    auto& frame = ctx.top();
    frame.push(std::make_shared<std::vector<anzu::object>>());
}

auto builtin_list_push(anzu::context& ctx) -> void
{
    auto& frame = ctx.top();
    verify(frame.stack_size() >= 2, "stack must contain two elements for list_push\n");
    verify(frame.top(1).is_list(), "second element on stack must be a list for list_push\n");
    auto elem = frame.pop();
    auto list = frame.pop();
    list.as_list()->push_back(elem);
}

auto builtin_list_pop(anzu::context& ctx) -> void
{
    auto& frame = ctx.top();
    verify(frame.top().is_list(), "top element on stack must be a list for list_pop\n");
    auto list = frame.pop();
    list.as_list()->pop_back();
}

auto builtin_list_size(anzu::context& ctx) -> void
{
    auto& frame = ctx.top();
    verify(frame.top().is_list(), "top element on stack must be a list for list_size\n");
    auto list = frame.pop();
    frame.push(static_cast<int>(list.as_list()->size()));
}

auto builtin_list_at(anzu::context& ctx) -> void
{
    auto& frame = ctx.top();
    verify(frame.stack_size() >= 2, "stack must contain two elements for list_push\n");
    verify(frame.top(0).is_int(), "first element of stack must be an integer (index into list)\n");
    verify(frame.top(1).is_list(), "second element on stack must be a list for list_push\n");
    auto pos = frame.pop();
    auto list = frame.pop();
    frame.push(list.as_list()->at(static_cast<std::size_t>(pos.to_int())));
}

}

static const std::unordered_map<std::string, std::function<void(anzu::context&)>> builtins = {
    { "print", builtin_print },
    { "stack_size", builtin_stack_size },

    // List functions
    { "list_new", builtin_list_new },
    { "list_push", builtin_list_push },
    { "list_pop", builtin_list_pop },
    { "list_size", builtin_list_size },
    { "list_at", builtin_list_at },

    // Debug functions
    { "__print_frame__", builtin_print_frame }
};

namespace anzu {

auto is_builtin(const std::string& name) -> bool
{
    return builtins.contains(name);
}

auto call_builtin(const std::string& name, anzu::context& ctx) -> void
{
    builtins.at(name)(ctx);
    ctx.top().ptr() += 1;
}

}