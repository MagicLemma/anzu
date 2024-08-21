#pragma once
#include "ast.hpp"
#include "parser.hpp"
#include "bytecode.hpp"
#include "names.hpp"

#include "compilation/type_manager.hpp"
#include "compilation/variable_manager.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace anzu {

struct signature
{
    std::vector<type_name> params;
    type_name              return_type;
};

struct function
{
    function_name    name;
    std::size_t      id;
    variable_manager variables;
    
    std::vector<std::byte> code = {};
    signature              sig  = {};
};

struct module_info
{
    std::filesystem::path                                  filepath;
    std::unordered_map<std::string, std::filesystem::path> imports;
};

struct struct_info
{
    type_struct  name;
    template_map templates;
};

struct func_info
{
    std::size_t  id;
    template_map templates;
};

struct compiler
{
    std::vector<function> functions;
    std::string           rom;

    type_manager types;

    std::unordered_set<std::filesystem::path> modules;

    std::unordered_map<function_name, std::size_t> functions_by_name;
    
    std::unordered_map<template_function_name, node_function_stmt> function_templates;
    std::unordered_map<template_struct_name, node_struct_stmt>     struct_templates;

    std::vector<module_info> current_module;
    std::vector<struct_info> current_struct;
    std::vector<func_info>   current_function;
};

auto compile(const anzu_module& ast) -> bytecode_program;

}