// Building programs: finding clang, the object cache (D64), and running processes.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace calx {

enum class BuildMode { debug, release };

// A program, or a library a program opens while it runs (D123).
enum class BuildKind { program, library };

struct Toolchain {
    std::string clang;    // the clang executable
    bool lld = false;     // whether lld is next to it
    std::string identity; // what changes when clang does: its path, size and modification time
    std::string home;     // the Calx installation: runtime/ and std/ are under it
    std::string cache;    // the object cache
};

// The Calx home: CALX_HOME, or the source tree the compiler was built from. Returns it empty,
// with the reason in `error`, if it has no runtime.
std::string find_home(std::string& error);

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

// Runs a program with its standard input read from the file `input`, and its standard output and
// error written to the files `output` and `errors`.
int run_redirected(const std::vector<std::string>& argv, const std::string& input, const std::string& output,
                   const std::string& errors);

// Starts several programs at once and waits for all of them. Returns the first nonzero status.
int run_parallel(const std::vector<std::vector<std::string>>& commands);

bool read_file(const std::string& path, std::string& out);
bool write_file_atomic(const std::string& path, const std::string& data);
bool file_exists(const std::string& path);

struct BuildResult {
    bool ok = false;
    std::string exe;        // the linked program or library, in the cache
    bool reused = false;    // nothing was compiled or linked
    double compile_ms = 0;  // clang, including the runtime when it wasn't cached
    double link_ms = 0;
};

// Compiles each of `c_sources`, one per module, and links them with the runtime. Each object is
// cached under a hash of its C text, so only files whose text changed are compiled again, all at
// once (D64), and an unchanged program is reused whole. A library has no runtime of its own: it
// takes the program's when the program opens it (D123).
BuildResult build_program(const Toolchain& tc, const std::vector<std::string>& c_sources, BuildMode mode, BuildKind kind,
                          bool verbose);

// A hash of the compiler's own bytes, as 32 hex digits, kept in the cache under its size and time
// so it's worked out once per build of the compiler.
std::string compiler_hash(const Toolchain& tc);

// How long the cache keeps what a build used, whatever its limits, since another build that's
// running side by side may need it still.
extern const std::chrono::minutes kCacheGrace;

// What a built library's file name ends with: `.dll` or `.so`.
const char* library_suffix();

// Copies what `build_program` built to `out`, for `calx build`. On Windows its PDB goes beside it
// as `out.pdb`, so stack traces and debuggers still find it once the cache has let it go.
bool place_build(const std::string& built, const std::string& out, std::string& error);

}  // namespace calx
