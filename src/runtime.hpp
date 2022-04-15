#pragma once
#include "program.hpp"

#include <vector>
#include <utility>

namespace anzu {

struct runtime_context
{
    std::size_t prog_ptr = 0;
    std::size_t base_ptr = 0;
    std::vector<std::byte> memory;
};

auto run_program(const program& prog) -> void;
auto run_program_debug(const program& prog) -> void;

}