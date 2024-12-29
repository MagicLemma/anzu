#include "type_manager.hpp"
#include "object.hpp"

namespace anzu {

auto type_manager::add_type(const type_struct& name, const template_map& templates) -> bool
{
    if (d_classes.contains(name)) {
        return false;
    }
    d_classes.emplace(name, type_info{{}, templates});
    return true;
}

auto type_manager::add_field(const type_struct& name, const type_field& field) -> bool
{
    if (!d_classes.contains(name)) {
        return false;
    }
    d_classes[name].fields.push_back(field);
    return true;
}

auto type_manager::contains(const type_struct& type) const -> bool
{
    return d_classes.contains(type);
}

auto type_manager::size_of(const type_name& type) const -> std::size_t
{
    return std::visit(overloaded{
        [](type_null) {
            return std::size_t{1};
        },
        [](type_bool) {
            return std::size_t{1};
        },
        [](type_char) {
            return std::size_t{1};
        },
        [](type_i32) {
            return std::size_t{4};
        },
        [](type_i64) {
            return std::size_t{8};
        },
        [](type_u64) {
            return std::size_t{8};
        },
        [](type_f64) {
            return std::size_t{8};
        },
        [](type_arena) {
            return sizeof(std::byte*); // the runtime will store the arena separately
        },
        [](type_module) {
            return std::size_t{0};
        },
        [](type_type) {
            return std::size_t{0};
        },
        [&](const type_struct& t) -> std::size_t {
            if (!d_classes.contains(t)) {
                panic("unknown type '{}'", type);
            }
            auto size = std::size_t{0};
            for (const auto& field : fields_of(t)) {
                size += size_of(field.type);
            }
            return std::max(std::size_t{1}, size); // empty structs take up one byte
        },
        [&](const type_array& t) {
            return size_of(*t.inner_type) * t.count;
        },
        [](const type_ptr&) {
            return sizeof(std::byte*);
        },
        [](const type_span&) {
            return sizeof(std::byte*) + sizeof(std::size_t);
        },
        [&](const type_optional& t) {
            return size_of(*t.inner_type) + 1;
        },
        [](const type_function_ptr&) {
            return sizeof(std::byte*);
        },
        [](const type_bound_method&) {
            return sizeof(std::byte*); // pointer to the object, first arg to the function
        },
        [](const type_bound_method_template&) {
            return sizeof(std::byte*); // pointer to the object, first arg to the function
        },
        [](const type_function&) {
            return std::size_t{0};
        },
        [](const type_function_template&) {
            return std::size_t{0};
        },
        [](const type_struct_template&) {
            return std::size_t{0};
        },
        [](const type_placeholder&) {
            return std::size_t{0}; 
        }
    }, type);
}

auto type_manager::fields_of(const type_struct& t) const -> type_fields
{
    if (auto it = d_classes.find(t); it != d_classes.end()) {
        return it->second.fields;
    }
    return {};
}

auto type_manager::templates_of(const type_struct& t) const -> template_map
{
    if (auto it = d_classes.find(t); it != d_classes.end()) {
        return it->second.templates;
    }
    return {};
}

}