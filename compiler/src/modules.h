// The module loader: the main file and every module it imports, found by their names (D73).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "parser.h"

namespace calx {

class Loader;

// Loads a program's modules, and more of them later, for the imports in code that attributes
// generate (D124).
class ModuleLoader {
public:
    ModuleLoader(ParseContext ctx, Sources& sources, const std::string& home, const std::string& home_error);
    ~ModuleLoader();

    // What `load_program` does, below. Returns the modules in order.
    const std::vector<Module*>& load(std::uint32_t main_file);
    // Adds `items` to the end of module `m`, and loads the modules their imports name, so that
    // each module still comes after the ones it imports. Errors go to `ctx.diags`.
    void add_items(Module* m, const std::vector<Item*>& items);
    const std::vector<Module*>& modules() const;
    // The folder module names start from: the main file's, less the folders its `module` line names.
    std::string root() const;

private:
    std::unique_ptr<Loader> impl_;
};

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
