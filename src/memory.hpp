#pragma once
#include <span>
#include <utility>
#include <vector>
#include <map>

namespace anzu {

class memory_allocator
{
    std::vector<std::byte>*            d_memory;
    std::map<std::size_t, std::size_t> d_pools;
    std::size_t                        d_bytes_allocated = 0;

public:
    memory_allocator(std::vector<std::byte>& memory) : d_memory(&memory) {}

    auto allocate(std::size_t size) -> std::size_t;
    auto deallocate(std::size_t ptr, std::size_t size) -> void;

    auto bytes_allocated() const -> std::size_t;
};

}