// The parser: tokens to a syntax tree.
#pragma once

#include <vector>

#include "ast.h"
#include "lexer.h"
#include "source.h"

namespace calx {

struct ParseContext {
    const Sources& sources;
    Names& names;
    Arena& arena;
    Diagnostics& diags;
    // If set, where each token the parser takes as a binary operator starts, for `calx fmt`, which
    // spaces `a < b` but not `List<i32>`.
    std::vector<std::uint32_t>* binary_ops = nullptr;
};

// Parses one file. Syntax errors are reported to `ctx.diags`. The parser recovers at the next
// statement or item, so one run reports more than the first error.
Module* parse_file(ParseContext ctx, std::uint32_t file_id);

}  // namespace calx
