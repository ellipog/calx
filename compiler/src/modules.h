// The module loader: the main file and every module it imports, found by their names (D73).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "packages.h"
#include "parser.h"

namespace calx {

class Loader;

// Whether a program's imports can name packages (CLX-370): those its calx.toml lists, found from
// the calx.toml nearest the main file's folder, or from `from`.
struct PackageOptions {
    bool enabled = false;
    std::string from;
    // Any package the program has, not only those its calx.toml lists, for the program that runs
    // a program's attributes, which imports each one's module by its whole name.
    bool any = false;
};

// Loads a program's modules, and more of them later, for the imports in code that attributes
// generate (D124).
class ModuleLoader {
public:
    ModuleLoader(ParseContext ctx, Sources& sources, const std::string& home, const std::string& home_error);
    ~ModuleLoader();

    // Before `load`: lets imports name packages.
    void use_packages(const PackageOptions& options);
    // What `load_program` does, below. Returns the modules in order.
    const std::vector<Module*>& load(std::uint32_t main_file);
    // Adds `items` to the end of module `m`, and loads the modules their imports name, so that
    // each module still comes after the ones it imports. Errors go to `ctx.diags`.
    void add_items(Module* m, const std::vector<Item*>& items);
    const std::vector<Module*>& modules() const;
    // The folder module names start from: the main file's, less the folders its `module` line names.
    std::string root() const;
    // Each folder whose names an import looked through, to find its file with exactly its letters:
    // a file added, removed or renamed there could change which an import finds (CLX-242).
    std::vector<std::string> folders() const;
    // The packages the program uses, and the folder calx.toml was looked for from.
    const PackageSet& packages() const;
    std::string packages_from() const;

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
//
// With packages, module `p.x` is module `x` of the package `p` the calx.toml lists, the file
// `x.calx` under the folder the package's calx.toml is in, and module `p` is its `p.calx`. In a
// package's own modules, names are its own: `import x` is its `x.calx`.
std::vector<Module*> load_program(ParseContext ctx, Sources& sources, std::uint32_t main_file, const std::string& home,
                                  const std::string& home_error, const PackageOptions& packages = {});

}  // namespace calx
