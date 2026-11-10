// Building programs: finding clang, the object cache (D64), and running processes.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
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

// The compiler's own executable.
std::filesystem::path self_exe();

// The Calx home, which holds runtime/ and std/: CALX_HOME, or the installation the executable is
// in (the folder above its bin/), or the source tree the compiler was built from. Returns it
// empty, with the reason in `error`, if none has a runtime.
std::string find_home(std::string& error);

// Finds clang (CALX_CC, or `clang` on PATH), the Calx home (see find_home) and the cache
// (CALX_CACHE, or the platform's cache folder). On failure returns false and puts the reason in
// `error`.
bool find_toolchain(Toolchain& tc, std::string& error);

// The program `name` (with `.exe` on Windows) in the first folder on PATH that has it, or "".
std::string which(const std::string& name);

// The cache folder: CALX_CACHE, or `%LOCALAPPDATA%\calx\cache` on Windows and
// `$XDG_CACHE_HOME/calx` or `~/.cache/calx` on Linux. On Windows, one of more than 100 characters
// is given in the `\\?\` form, so the paths under it can pass MAX_PATH.
std::string cache_dir();

// `path` to open, make or remove: on Windows, in the `\\?\` form if it's 248 characters or more,
// since Windows' file functions refuse longer paths otherwise. Messages show the path as given.
std::filesystem::path long_path(const std::string& path);

// Starts a program that runs on after this one ends, with no console, no inherited handles and
// its streams going nowhere. Returns whether it started.
bool start_detached(const std::vector<std::string>& argv);

// A 128-bit hash as 32 hex digits, for cache keys.
std::string hash_hex(const std::vector<std::string_view>& parts);

// Runs a program and waits for it. Returns its exit status, or -1 if it couldn't start. With
// `name`, the program sees that as its `argv[0]`, rather than its path.
int run_process(const std::vector<std::string>& argv, const std::string& name = "");

// Sets this process's environment for the program `calx run` starts next: CALX, this compiler's
// path, for a script that builds or checks with the compiler that ran it, as Python's
// `sys.executable`, and CALX_SCRIPT, the full path of `source`, which is `std.env.script()`. The
// program's runtime takes CALX_SCRIPT out of its own environment as it starts, so what that
// program starts doesn't inherit it; CALX it does.
void set_run_environment(const std::string& source);

// Runs a program with its standard output and error going to the file `output`.
int run_to_file(const std::vector<std::string>& argv, const std::string& output);

// Runs a program with its standard input read from the file `input`, and its standard output and
// error written to the files `output` and `errors`.
int run_redirected(const std::vector<std::string>& argv, const std::string& input, const std::string& output,
                   const std::string& errors);

// Runs several programs at once, at most one for each logical processor on the machine at a time,
// counting those every other calx runs this way, and waits for all of them. Returns the first
// nonzero status, or -1 if one couldn't start.
int run_parallel(const std::vector<std::vector<std::string>>& commands);

bool read_file(const std::string& path, std::string& out);
bool write_file_atomic(const std::string& path, const std::string& data);
bool file_exists(const std::string& path);

// A name part no other build, in this process or another, uses at the same time, for the files a
// build writes in the shared cache.
std::string unique_stamp();

struct BuildResult {
    bool ok = false;
    std::string exe;        // the linked program or library, in the cache
    bool reused = false;    // nothing was compiled or linked
    double compile_ms = 0;  // clang, including the runtime when it wasn't cached
    double link_ms = 0;
};

// Compiles each of `c_sources`, one per module, and links them with the runtime. Each object is
// cached under a hash of its C text, so only files whose text changed are compiled again, in
// parallel (D64), at most one clang for each logical processor on the machine at a time
// (run_parallel, CLX-205), and an unchanged program is reused whole. A library has no runtime of
// its own: it takes the program's when the program opens it (D123).
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
