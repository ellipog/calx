// The type checker: resolves names, gives every expression a type, and enforces the language's
// rules, including the strictness rules of D63.
#pragma once

#include <unordered_map>
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

// The derived properties a struct or an enum can write its own of, in `Program::hand`'s order.
enum Property : int { prop_equal, prop_display, prop_hash, prop_order };

// What the emitter needs, once a program has checked without errors.
struct Program {
    std::vector<Module*> modules;  // each after the modules it imports, so the main module is last
    std::vector<Type*> type_order; // the types emitted, each after the types it holds by value
    std::vector<Item*> fns;        // every function with a body: the written ones, then the instances
    std::vector<Item*> consts;     // constants the emitter defines in C: the floats
    // For each `Property`, the types that write their own and the function that does it: `eq`,
    // `show`, `hash` or `cmp`, or its instance for a generic type (D102).
    std::unordered_map<const Type*, Item*> hand[4];
    const Type* hasher = nullptr;  // `std.core`'s `Hasher`, what a hand-written `hash` adds to
    Item* main = nullptr;
};

// Checks a program: its modules, from `load_program`, as one. With `need_main`, the main module
// must have `fn main()`. Errors go to `ctx.diags`.
void check_program(CheckContext ctx, const std::vector<Module*>& modules, Program& program, bool need_main);

}  // namespace calx
