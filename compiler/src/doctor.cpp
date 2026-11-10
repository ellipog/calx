#include "doctor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "term.h"
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

// In a terminal, a check's row: its mark, a label in a column of its own, then what it found,
// with more under it in grey. Labels take kLabel columns, so what's found starts at kColumn.
constexpr int kLabel = 14;
constexpr int kColumn = 4 + kLabel;

// A check's line: `  ok    plain`, or in a terminal its row, with ✓, ! for a warning or ✗ for a
// failure.
void line(const char* status, const std::string& plain, const std::string& label, const std::string& found,
          const std::string& more) {
    const term::Caps& c = term::out();
    if (!c.color) {
        std::printf("  %-5s %s\n", status, plain.c_str());
        return;
    }
    std::string mark = status[0] == 'o' ? term::tick(c) : status[0] == 'w' ? term::bang(c) : term::cross(c);
    int room = std::max(30, c.columns - kColumn - 1);
    std::vector<std::string> lines = term::width(found) <= room ? std::vector<std::string>{found} : term::wrap(found, room);
    for (std::size_t i = 0; i < lines.size(); i++) {
        std::string left = i == 0 ? "  " + mark + " " + term::bold(c, label) : "";
        std::printf("%s%s\n", term::pad(left, kColumn).c_str(), term::code(c, lines[i], false).c_str());
    }
    if (!more.empty()) std::printf("%s%s\n", std::string(kColumn, ' ').c_str(), term::dim(c, more).c_str());
    std::fflush(stdout);  // each line as it's found, before a status line takes its place
}
void ok(const std::string& plain, const std::string& label, const std::string& found, const std::string& more = "") {
    line("ok", plain, label, found, more);
}
void warn(const std::string& plain, const std::string& label, const std::string& found, const std::string& more = "") {
    line("warn", plain, label, found, more);
}
void fail(const std::string& plain, const std::string& label, const std::string& found, const std::string& more = "") {
    line("FAIL", plain, label, found, more);
    failures++;
}
// How to fix what the line above says, indented under it, and in a terminal under what it found,
// after an amber ›.
void fix(const std::string& how) {
    const term::Caps& c = term::out();
    if (c.color) {
        // One paragraph, wrapped to the terminal: its line ends are for the plain output's width.
        std::string flat = how;
        for (char& ch : flat) if (ch == '\n') ch = ' ';
        std::vector<std::string> lines = term::wrap(flat, std::max(30, c.columns - kColumn - 1));
        for (std::size_t i = 0; i < lines.size(); i++) {
            std::printf("%s%s %s\n", std::string(kColumn - 2, ' ').c_str(), i == 0 ? term::accent(c, "\xE2\x80\xBA").c_str() : " ",
                        term::code(c, lines[i], false).c_str());
        }
        return;
    }
    std::size_t at = 0;
    while (at <= how.size()) {
        std::size_t end = how.find('\n', at);
        if (end == std::string::npos) end = how.size();
        std::printf("          %s\n", how.substr(at, end - at).c_str());
        at = end + 1;
    }
}

// In a terminal, a heading over the checks that follow.
void section(const char* title) {
    const term::Caps& c = term::out();
    if (c.color) std::printf("\n  %s\n", term::heading(c, title).c_str());
}

// While a check runs, in a terminal, its row with a pulse for its mark and a scanner where what it
// finds will go, which the row it finds takes the place of. A check that's quick never shows it.
void checking(std::optional<term::Status>& s, const std::string& label) {
    if (!term::out().live) return;
    s.emplace(
        [label](int frame, double seconds) {
            const term::Caps& e = term::err();
            static const char* const kPulse[] = {"\xE2\x96\x91", "\xE2\x96\x92", "\xE2\x96\x93", "\xE2\x96\x88", "\xE2\x96\x93", "\xE2\x96\x92"};
            return term::pad("  " + term::accent(e, kPulse[frame % 6]) + " " + term::bold(e, label), kColumn) +
                   term::scanner(e, frame, 12) + (seconds >= 1 ? "  " + term::dim(e, term::elapsed(seconds)) : "");
        },
        std::chrono::milliseconds(40));
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

// `text` without the spaces and line ends around it.
std::string trimmed(const std::string& text) {
    std::size_t a = text.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return text.substr(a, text.find_last_not_of(" \t\r\n") - a + 1);
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
    // A release builds with its own clang in toolchain/ (CLX-110); a build from source uses one
    // installed.
    if (CALX_COMMIT[0]) {
        return "`calx update --toolchain` installs the clang a release of Calx builds with, in toolchain/\n"
               "beside bin/. Or set CALX_CC to another clang.";
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
    const term::Caps& c = term::out();
    if (c.color) {
        std::printf("\n%s", term::beside_mark(c, {"", "", term::bold(c, "calx doctor"),
                                                  term::dim(c, "checks that Calx can build and run programs,"),
                                                  term::dim(c, "and says how to fix what's wrong"), "", ""})
                                 .c_str());
    } else {
        std::printf("calx doctor\n\n");
    }
    std::error_code ec;
    fs::path self = self_exe();

    // This calx, and the runtime and std it uses.
    section("calx");
    if (CALX_COMMIT[0]) {
        ok(std::string("calx ") + CALX_VERSION + " (" + CALX_COMMIT + ", " + CALX_DATE + ") at " + self.string(), "calx",
           std::string(CALX_VERSION) + term::dim(c, std::string("  ") + CALX_COMMIT + ", " + CALX_DATE), self.string());
    } else {
        ok(std::string("calx ") + CALX_VERSION + ", built from source, at " + self.string(), "calx",
           std::string(CALX_VERSION) + term::dim(c, "  built from source"), self.string());
    }
    std::string home_error;
    std::string home = find_home(home_error);
    if (home.empty()) {
        fail("no runtime or standard library: " + home_error, "runtime, std", "not found: " + home_error);
        fix("Reinstall Calx, or set CALX_HOME to the folder that holds runtime/ and std/.");
    } else if (!fs::is_regular_file(fs::path(home) / "std" / "io.calx", ec)) {
        fail("the standard library isn't in " + home + "/std", "runtime, std", "the standard library isn't in " + home + "/std");
        fix("Reinstall Calx.");
    } else {
        ok("runtime and standard library in " + home, "runtime, std", home);
    }

    // The toolchain the release builds programs with, a download of its own that the install
    // scripts and `calx update` fetch, named on the third line of toolchain.txt (CLX-251).
    section("toolchain");
    std::string note;
    if (!home.empty() && read_file((fs::path(home) / "toolchain.txt").string(), note)) {
        std::string name, installed;
        std::size_t at = 0;
        for (int line = 0; line < 3 && at <= note.size(); line++) {
            std::size_t end = note.find('\n', at);
            if (end == std::string::npos) end = note.size();
            if (line == 2) name = trimmed(note.substr(at, end - at));
            at = end + 1;
        }
        read_file((fs::path(home) / "toolchain" / "VERSION").string(), installed);
        installed = trimmed(installed);
        if (installed.empty()) {
            warn("the toolchain this release builds programs with, " + name + ", isn't installed", "toolchain",
                 name + " isn't installed");
            fix("`calx update --toolchain` installs it.");
        } else if (installed != name) {
            warn("the toolchain installed is " + installed + ", but this release builds with " + name, "toolchain",
                 installed + " is installed, but this release builds with " + name);
            fix("`calx update --toolchain` installs that one.");
        }
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
        fail("no clang: Calx builds programs with it", "clang", "not found: Calx builds programs with it");
        fix(install_clang());
    } else {
        std::string out;
        int status;
        {
            std::optional<term::Status> pending;
            checking(pending, "clang");
            status = capture({clang, "--version"}, folder, out);
        }
        std::string version = clang_version(first_line(out));
        int major = std::atoi(version.c_str());
        if (status != 0) {
            fail("clang at " + clang + " doesn't run", "clang", "doesn't run", clang);
            fix(install_clang());
        } else if (major > 0 && major < 18) {
            fail("clang " + version + " at " + clang + ", but Calx needs LLVM 18 or later", "clang",
                 version + ", but Calx needs LLVM 18 or later", clang);
            fix(install_clang());
        } else {
            std::string which_one = from_env ? " (from CALX_CC)" : bundled ? " (the one Calx carries)" : "";
            std::string target = tc.mingw ? ", building for mingw-w64 and the UCRT" : "";
            std::string shown = version.empty() ? first_line(out) : version;
            ok("clang " + shown + " at " + clang + which_one + target, "clang",
               shown + term::dim(c, (from_env ? "  from CALX_CC" : bundled ? "  the one Calx carries" : "") +
                                        std::string(tc.mingw ? ", for mingw-w64 and the UCRT" : "")),
               clang);
            have_clang = true;
        }
        bool lld = found && tc.lld;
        if (have_clang && lld) {
            ok("lld, which links faster and optimizes across modules in release builds", "lld",
               "links faster, and optimizes across modules in release builds");
        } else if (have_clang) {
            warn("no lld beside clang: builds link slower, and release builds can't inline across modules", "lld",
                 "not beside clang: builds link slower, and release builds can't inline across modules");
            fix("lld comes with LLVM; install it beside clang.");
        }
    }

    // A program, built and run in each mode, which is what everything else is for.
    if (have_clang && !home.empty()) {
        section("builds");
        std::string source = (fs::path(folder) / "doctor.calx").string();
        write_file_atomic(source, "fn main() {\n    println(\"calx works\")\n}\n");
        for (const char* mode : {"debug", "release"}) {
            std::vector<std::string> argv = {self.string(), "run", source};
            if (std::string(mode) == "release") argv.insert(argv.begin() + 2, "--release");
            auto t0 = std::chrono::steady_clock::now();
            std::string out;
            int status;
            {
                std::optional<term::Status> pending;
                checking(pending, mode);
                status = capture(argv, folder, out);
            }
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (status == 0 && contains(out, "calx works")) {
                char buf[64];
                std::snprintf(buf, sizeof buf, " (%.0f ms)", ms);
                ok(std::string("builds and runs a program in a ") + mode + " build" + buf, mode,
                   "builds and runs a program" + term::dim(c, "  " + term::duration(ms / 1000)));
            } else {
                fail(std::string("can't build a program in a ") + mode + " build", mode, "can't build a program");
                // The last lines of what went wrong.
                std::size_t from = out.size() > 1500 ? out.size() - 1500 : 0;
                std::string tail = out.substr(from);
                if (c.color) {
                    std::size_t at = 0;
                    while (at < tail.size()) {
                        std::size_t end = tail.find('\n', at);
                        if (end == std::string::npos) end = tail.size();
                        std::printf("    %s %s\n", term::dim(c, "\xE2\x94\x82").c_str(), tail.substr(at, end - at).c_str());
                        at = end + 1;
                    }
                } else {
                    std::printf("%s\n", tail.c_str());
                }
                fix(diagnose(out, tc.mingw));
                break;
            }
        }
    }
    fs::remove_all(folder, ec);

    // PATH.
    section("setup");
    std::string on_path = which("calx");
    if (on_path.empty()) {
        warn("calx isn't on PATH", "PATH", "calx isn't on it");
        fix("Add " + self.parent_path().string() + " to PATH, or run the install script again.");
    } else if (fs::equivalent(fs::path(on_path), self, ec)) {
        ok("calx on PATH is this one", "PATH", "this calx is the one on it");
    } else {
        warn("calx on PATH is another one, at " + on_path, "PATH", "the calx on it is another one", on_path);
        fix("Put " + self.parent_path().string() + " first on PATH, or remove the other.");
    }

    // The cache.
    fs::path cache = cache_dir();
    std::uintmax_t bytes;
    {
        std::optional<term::Status> pending;
        checking(pending, "build cache");
        bytes = folder_bytes(cache);
    }
    char size[32];
    std::snprintf(size, sizeof size, "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    char shown[32];
    if (bytes >= 1024ull * 1024 * 1024) std::snprintf(shown, sizeof shown, "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    else std::snprintf(shown, sizeof shown, "%s", size);
    ok("build cache in " + cache.string() + ", " + size, "build cache", shown, cache.string());

    // Updates.
    if (!CALX_COMMIT[0]) {
        ok("updates: a build from source updates with `git pull` and a rebuild", "updates",
           "a build from source updates with `git pull` and a rebuild");
    } else if (!fs::is_regular_file(self.parent_path() / (std::string("calx-update") + kExe), ec)) {
        warn("updates: calx-update isn't beside calx, so `calx update` can't run", "updates",
             "calx-update isn't beside calx, so `calx update` can't run");
        fix("Reinstall Calx.");
    } else {
        std::string text;
        read_file((cache / "update" / "latest").string(), text);
        std::size_t a = text.find('\n');
        std::size_t b = a == std::string::npos ? a : text.find('\n', a + 1);
        std::string newer = b == std::string::npos ? "" : text.substr(a + 1, b - a - 1);
        if (text.substr(0, a) == CALX_VERSION && !newer.empty()) {
            warn("updates: calx " + newer + " is out", "updates", "calx " + newer + " is out");
            fix("`calx update` installs it.");
        } else {
            ok("updates: `calx update` installs new releases, and a daily check says when there's one", "updates",
               "`calx update` installs new releases, and a daily check says when there's one");
        }
    }

    if (c.color) {
        std::printf("\n  %s\n\n", failures ? term::band(c, "\xE2\x9C\x97 Something needs fixing: see above", term::Tone::bad).c_str()
                                          : term::band(c, "\xE2\x9C\x93 Everything works", term::Tone::good).c_str());
    } else {
        std::printf("\n%s\n", failures ? "Something needs fixing: see above." : "Everything works.");
    }
    return failures ? 1 : 0;
}

}  // namespace calx
