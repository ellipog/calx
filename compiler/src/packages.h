// Packages (CLX-370): a folder with a `calx.toml` giving its name, version and dependencies, by
// local path or by git URL, which calx.lock pins to commits. A dependency's modules are found
// under its own folder, and a program imports them under the dependency's name: module `x.y` of
// package `spar` is `spar.x.y`, and its file `spar.calx` is `spar`.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "source.h"

namespace calx {

struct Package {
    std::string name;
    std::string version;
    // The folder its calx.toml is in, where its modules are found: as a path from the current
    // folder when it's under it, and otherwise whole.
    std::string dir;
    // How it was found, for messages: `../util`, or `https://host/spar.git` at tag `v1.0.0`.
    std::string source;
    std::map<std::string, std::size_t> deps;  // each dependency's name, and its index
};

struct PackageSet {
    // The packages, the one whose calx.toml was found first. Empty when there's no calx.toml.
    std::vector<Package> packages;
    // Each place a calx.toml was looked for and none was, and each file this wrote, calx.lock,
    // for the build's record (CLX-242). The calx.toml and calx.lock files read are in Sources.
    std::vector<std::string> missing;
    std::vector<std::string> written;
    std::string lock;  // the main package's calx.lock, whether it's there or not

    bool any() const { return !packages.empty(); }
    // The package of that name, or -1.
    int find(const std::string& name) const;
};

// Looks for the calx.toml nearest `from`, a folder, in it and the folders above it, and reads it
// and the packages it depends on, directly or not. A git dependency calx.lock pins is used from the
// cache as it is, with no git at all; one it doesn't, or a commit not yet fetched, is fetched with
// `git` from PATH, and calx.lock is written with what it was pinned to. Errors, such as a missing
// field, a path that isn't there or two packages of one name, go to `diags`. With no calx.toml,
// `out` is left with no packages and nothing is an error.
void load_packages(Sources& sources, Diagnostics& diags, const std::string& from, PackageSet& out);

}  // namespace calx
