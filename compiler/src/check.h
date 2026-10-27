// The type checker: resolves names, gives every expression a type, and enforces the language's
// rules, including the strictness rules of D63.
#pragma once

#include <vector>

#include "ast.h"
#include "source.h"
#include "types.h"

namespace calx {

struct CheckContext {
    const Sources& sources;
    Names& names;
    Arena& arena;
    Diagnostics& diags;
    Types& types;
};

// What the emitter needs, once a module has checked without errors.
struct Program {
    Module* root = nullptr;
    std::string_view prefix;       // the C names of the module's items start with this
    std::vector<Type*> type_order; // structs and arrays, each after the types it holds by value
    std::vector<Item*> fns;        // every function with a body, in source order
    std::vector<Item*> consts;     // constants the emitter defines in C: the floats
    Item* main = nullptr;
};

// Checks one module. With `need_main`, the module must have `fn main()`. Errors go to `ctx.diags`.
void check_module(CheckContext ctx, Module* module, Program& program, bool need_main);

}  // namespace calx
