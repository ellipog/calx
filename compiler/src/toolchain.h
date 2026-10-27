// Building programs: finding clang, the object cache (D64), and running processes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace calx {

enum class BuildMode { debug, release };

struct Toolchain {
    std::string clang;    // the clang executable
    bool lld = false;     // whether lld is next to it
    std::string identity; // what changes when clang does: its path, size and modification time
    std::string home;     // the Calx installation: runtime/ and std/ are under it
    std::string cache;    // the object cache
};

// Finds clang (CALX_CC, or `clang` on PATH), the Calx home (CALX_HOME, or the source tree the
// compiler was built from) and the cache (CALX_CACHE, or the platform's cache folder). On failure
// returns false and puts the reason in `error`.
bool find_toolchain(Toolchain& tc, std::string& error);

// A 128-bit hash as 32 hex digits, for cache keys.
std::string hash_hex(const std::vector<std::string_view>& parts);

// Runs a program and waits for it. Returns its exit status, or -1 if it couldn't start.
int run_process(const std::vector<std::string>& argv);

// Runs a program with its standard output and error going to the file `output`.
int run_to_file(const std::vector<std::string>& argv, const std::string& output);

// Starts several programs at once and waits for all of them. Returns the first nonzero status.
int run_parallel(const std::vector<std::vector<std::string>>& commands);

bool read_file(const std::string& path, std::string& out);
bool write_file_atomic(const std::string& path, const std::string& data);
bool file_exists(const std::string& path);

struct BuildResult {
    bool ok = false;
    std::string exe;        // the linked program, in the cache
    bool reused = false;    // nothing was compiled or linked
    double compile_ms = 0;  // clang, including the runtime when it wasn't cached
    double link_ms = 0;
};

// Compiles `c_source` and links it with the runtime, reusing cached objects and executables.
BuildResult build_program(const Toolchain& tc, const std::string& c_source, BuildMode mode, bool verbose);

}  // namespace calx
