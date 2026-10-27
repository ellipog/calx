// The C emitter: a checked program to C files, one per module, which include only the runtime's header.
#pragma once

#include <string>
#include <vector>

#include "check.h"

namespace calx {

struct EmitOptions {
    bool line_directives = true;  // `#line`, so debuggers and profilers show Calx lines (P3.10)
};

// One C file of a program: a module's functions and what they need (D75).
struct CFile {
    std::string name;  // after the module, as in `geo_shapes.c`
    std::string text;
};

// The C files for a checked program, one per module that has functions, the main module last.
std::vector<CFile> emit_c(const Program& program, const Sources& sources, const Names& names, const EmitOptions& options);

}  // namespace calx
