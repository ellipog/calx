#include "doctor.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "toolchain.h"

namespace fs = std::filesystem;

namespace calx {

namespace {

#ifdef _WIN32
constexpr const char* kExe = ".exe";
#else
constexpr const char* kExe = "";
#endif

int failures = 0;

void line(const char* status, const std::string& what) { std::printf("  %-5s %s\n", status, what.c_str()); }
void ok(const std::string& what) { line("ok", what); }
void warn(const std::string& what) { line("warn", what); }
void fail(const std::string& what) {
    line("FAIL", what);
    failures++;
}
// How to fix what the line above says, indented under it.
void fix(const std::string& how) {
    std::size_t at = 0;
    while (at <= how.size()) {
        std::size_t end = how.find('\n', at);
        if (end == std::string::npos) end = how.size();
        std::printf("          %s\n", how.substr(at, end - at).c_str());
        at = end + 1;
    }
}

bool contains(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

std::string temp_folder() {
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec) base = fs::path(cache_dir());
    fs::path dir = base / ("calx-doctor-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir, ec);
    return dir.string();
}

// What a program prints, standard output and error together, and its exit status.
int capture(const std::vector<std::string>& argv, const std::string& folder, std::string& out) {
    std::string file = (fs::path(folder) / "out.txt").string();
    int status = run_to_file(argv, file);
    out.clear();
    read_file(file, out);
    return status;
}

// The first line of `text`.
std::string first_line(const std::string& text) {
    std::size_t end = text.find_first_of("\r\n");
    return text.substr(0, end);
}

// The version clang's first line names, such as 23.1.4, or "".
std::string clang_version(const std::string& banner) {
    std::size_t at = banner.find("version ");
    if (at == std::string::npos) return {};
    at += 8;
    std::size_t end = at;
    while (end < banner.size() && (std::isdigit(static_cast<unsigned char>(banner[end])) || banner[end] == '.')) end++;
    return banner.substr(at, end - at);
}

std::uintmax_t folder_bytes(const fs::path& dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

std::string install_clang() {
    // A release carries its own clang in toolchain/ (CLX-110); a build from source uses one installed.
    if (CALX_COMMIT[0]) {
        return "A release of Calx carries its own clang, in toolchain/ beside bin/. Reinstall Calx to get\n"
               "it back, or set CALX_CC to another clang.";
    }
#ifdef _WIN32
    return "Install LLVM with `winget install LLVM.LLVM`, then open a new terminal, or set CALX_CC to\n"
           "the full path of clang.exe.";
#else
    return "Install clang and lld: on Ubuntu or Debian, `sudo apt install clang lld`. If that clang is\n"
           "older than 18, apt.llvm.org has newer ones. Or set CALX_CC to a clang.";
#endif
}

// What a failed build's output says about what's missing, and how to get it. `mingw` says clang
// builds for mingw-w64 with the headers and libraries beside it, rather than Visual Studio's.
std::string diagnose(const std::string& out, bool mingw) {
#ifdef _WIN32
    if (mingw && (contains(out, "file not found") || contains(out, "unable to find library") ||
                  contains(out, "crt2.o") || contains(out, "libmingw"))) {
        return "clang can't find mingw-w64's headers or libraries beside it, in the folder above its bin/.\n"
               "Reinstall Calx, or set CALX_CC to another clang.";
    }
    if (contains(out, "libcmt") || contains(out, "msvcrt") || contains(out, "kernel32.lib") || contains(out, "vcruntime") ||
        contains(out, "'stdio.h' file not found") || contains(out, "ucrt")) {
        return "clang can't find the Windows SDK or the C runtime it links programs with. Install Visual\n"
               "Studio's Build Tools with the \"Desktop development with C++\" workload, from\n"
               "https://visualstudio.microsoft.com/downloads/, then open a new terminal.";
    }
#else
    (void)mingw;
    if (contains(out, "crt1.o") || contains(out, "-lc") || contains(out, "'stdio.h' file not found") ||
        contains(out, "cannot find -lgcc")) {
        return "clang can't find the C library's headers or start files. On Ubuntu or Debian:\n"
               "`sudo apt install libc6-dev`, or `sudo apt install build-essential`.";
    }
#endif
    if (contains(out, "lld") || contains(out, "LTO") || contains(out, "-flto")) {
        return "Release builds link with lld and ThinLTO. Install lld beside clang: it comes with LLVM.";
    }
    return "What clang said is above. If it's not clear, report it at https://github.com/ellipog/calx/issues.";
}

}  // namespace

int run_doctor() {
    std::printf("calx doctor\n\n");
    std::error_code ec;
    fs::path self = self_exe();

    // This calx, and the runtime and std it uses.
    if (CALX_COMMIT[0]) {
        ok(std::string("calx ") + CALX_VERSION + " (" + CALX_COMMIT + ", " + CALX_DATE + ") at " + self.string());
    } else {
        ok(std::string("calx ") + CALX_VERSION + ", built from source, at " + self.string());
    }
    std::string home_error;
    std::string home = find_home(home_error);
    if (home.empty()) {
        fail("no runtime or standard library: " + home_error);
        fix("Reinstall Calx, or set CALX_HOME to the folder that holds runtime/ and std/.");
    } else if (!fs::is_regular_file(fs::path(home) / "std" / "io.calx", ec)) {
        fail("the standard library isn't in " + home + "/std");
        fix("Reinstall Calx.");
    } else {
        ok("runtime and standard library in " + home);
    }

    // clang and lld, found as a build finds them: CALX_CC, then the toolchain the installation
    // carries, then PATH.
    const char* cc = std::getenv("CALX_CC");
    bool from_env = cc && *cc;
    Toolchain tc;
    std::string tc_error;
    bool found = !home.empty() && find_toolchain(tc, tc_error);
    std::string clang = found ? tc.clang : from_env ? std::string(cc) : which("clang");
    bool bundled = found && !from_env &&
                   fs::equivalent(fs::path(clang).parent_path().parent_path(), fs::path(home) / "toolchain", ec);
    std::string folder = temp_folder();
    bool have_clang = false;
    if (clang.empty()) {
        fail("no clang: Calx builds programs with it");
        fix(install_clang());
    } else {
        std::string out;
        int status = capture({clang, "--version"}, folder, out);
        std::string version = clang_version(first_line(out));
        int major = std::atoi(version.c_str());
        if (status != 0) {
            fail("clang at " + clang + " doesn't run");
            fix(install_clang());
        } else if (major > 0 && major < 18) {
            fail("clang " + version + " at " + clang + ", but Calx needs LLVM 18 or later");
            fix(install_clang());
        } else {
            ok("clang " + (version.empty() ? first_line(out) : version) + " at " + clang +
               (from_env ? " (from CALX_CC)" : bundled ? " (the one Calx carries)" : "") +
               (tc.mingw ? ", building for mingw-w64 and the UCRT" : ""));
            have_clang = true;
        }
        bool lld = found && tc.lld;
        if (have_clang && lld) {
            ok("lld, which links faster and optimizes across modules in release builds");
        } else if (have_clang) {
            warn("no lld beside clang: builds link slower, and release builds can't inline across modules");
            fix("lld comes with LLVM; install it beside clang.");
        }
    }

    // A program, built and run in each mode, which is what everything else is for.
    if (have_clang && !home.empty()) {
        std::string source = (fs::path(folder) / "doctor.calx").string();
        write_file_atomic(source, "fn main() {\n    println(\"calx works\")\n}\n");
        for (const char* mode : {"debug", "release"}) {
            std::vector<std::string> argv = {self.string(), "run", source};
            if (std::string(mode) == "release") argv.insert(argv.begin() + 2, "--release");
            auto t0 = std::chrono::steady_clock::now();
            std::string out;
            int status = capture(argv, folder, out);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (status == 0 && contains(out, "calx works")) {
                char buf[64];
                std::snprintf(buf, sizeof buf, " (%.0f ms)", ms);
                ok(std::string("builds and runs a program in a ") + mode + " build" + buf);
            } else {
                fail(std::string("can't build a program in a ") + mode + " build");
                // The last lines of what went wrong.
                std::size_t from = out.size() > 1500 ? out.size() - 1500 : 0;
                std::printf("%s\n", out.substr(from).c_str());
                fix(diagnose(out, tc.mingw));
                break;
            }
        }
    }
    fs::remove_all(folder, ec);

    // PATH.
    std::string on_path = which("calx");
    if (on_path.empty()) {
        warn("calx isn't on PATH");
        fix("Add " + self.parent_path().string() + " to PATH, or run the install script again.");
    } else if (fs::equivalent(fs::path(on_path), self, ec)) {
        ok("calx on PATH is this one");
    } else {
        warn("calx on PATH is another one, at " + on_path);
        fix("Put " + self.parent_path().string() + " first on PATH, or remove the other.");
    }

    // The cache.
    fs::path cache = cache_dir();
    std::uintmax_t bytes = folder_bytes(cache);
    char size[32];
    std::snprintf(size, sizeof size, "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    ok("build cache in " + cache.string() + ", " + size);

    // Updates.
    if (!CALX_COMMIT[0]) {
        ok("updates: a build from source updates with `git pull` and a rebuild");
    } else if (!fs::is_regular_file(self.parent_path() / (std::string("calx-update") + kExe), ec)) {
        warn("updates: calx-update isn't beside calx, so `calx update` can't run");
        fix("Reinstall Calx.");
    } else {
        std::string text;
        read_file((cache / "update" / "latest").string(), text);
        std::size_t a = text.find('\n');
        std::size_t b = a == std::string::npos ? a : text.find('\n', a + 1);
        std::string newer = b == std::string::npos ? "" : text.substr(a + 1, b - a - 1);
        if (text.substr(0, a) == CALX_VERSION && !newer.empty()) {
            warn("updates: calx " + newer + " is out");
            fix("`calx update` installs it.");
        } else {
            ok("updates: `calx update` installs new releases, and a daily check says when there's one");
        }
    }

    std::printf("\n%s\n", failures ? "Something needs fixing: see above." : "Everything works.");
    return failures ? 1 : 0;
}

}  // namespace calx
