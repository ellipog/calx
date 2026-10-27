// The module loader: the main file and every module it imports, found by their names (D73).
#pragma once

#include <vector>

#include "parser.h"

namespace calx {

// Parses `main_file` and every module it imports, directly or not. Module `a.b.c` is the file
// `a/b/c.calx` under the source root: the main file's folder, less the folders its own `module`
// line names. Returns the modules with each after the ones it imports, so the main module is
// last. Errors, such as a missing module or an import cycle, go to `ctx.diags`.
//
// Module `std.x` is `std/x.calx` under `home`, the Calx installation, wherever the program is.
// With `home` empty, importing one is an error saying why: `home_error`.
std::vector<Module*> load_program(ParseContext ctx, Sources& sources, std::uint32_t main_file, const std::string& home,
                                  const std::string& home_error);

}  // namespace calx
