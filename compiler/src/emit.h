// The C emitter: a checked program to one C file, which includes only the runtime's header.
#pragma once

#include <string>

#include "check.h"

namespace calx {

struct EmitOptions {
    bool line_directives = true;  // `#line`, so debuggers and profilers show Calx lines (P3.10)
};

std::string emit_c(const Program& program, const Sources& sources, const Names& names, const EmitOptions& options);

}  // namespace calx
