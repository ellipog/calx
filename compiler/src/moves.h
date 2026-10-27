// The ownership pass (D76). After a function's body is checked, it follows each value that isn't
// `Copy` through the body: it reports a use after the value moved away, and works out what's
// dropped where, which it records in the tree for the emitter.
#pragma once

#include "ast.h"
#include "source.h"
#include "types.h"

namespace calx {

struct MoveContext {
    Names& names;
    Arena& arena;
    Diagnostics& diags;
};

// `report` is false for a generic function's instances, whose definition reported its errors.
void check_moves(MoveContext ctx, Item* fn, bool report);

}  // namespace calx
