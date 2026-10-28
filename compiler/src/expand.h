// Library-defined attributes (D57, D124): finding where a program uses them, building and running
// them, and adding the code they generate to the program before it's checked.
#pragma once

#include <string>
#include <vector>

#include "modules.h"

namespace calx {

// What one use of an attribute generated, for `calx expand`.
struct Expansion {
    std::string what;  // "`@packet` on `Intention`, at proto.calx:12"
    std::string path;  // the file the code is in, in the cache
    std::string code;
};

struct ExpandOptions {
    std::string home, home_error;
    bool verbose = false;  // print the clang commands that build the attributes
    bool json = false;     // print errors in the attributes' own code as JSON lines
    bool color = false;
    std::vector<Expansion>* shown = nullptr;  // filled in with what each use generated
};

// Runs the attributes the modules use, which libraries define as Calx functions marked
// `@attribute`, and adds the code each use gives back to the module it's in. The compiler builds
// the attributes into a program of their own, with the same cache as any build, and gives it the
// declarations as data; what it answers is cached under all it was given, so an unchanged
// program runs nothing. Returns false if it couldn't: the errors are in `ctx.diags`, and errors
// in the attributes' own code have been printed already.
bool expand_attributes(ParseContext ctx, Sources& sources, ModuleLoader& loader, const ExpandOptions& options);

}  // namespace calx
