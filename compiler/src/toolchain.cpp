#include "toolchain.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace calx {

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

#ifdef _WIN32
constexpr char kPathSep = ';';
constexpr const char* kExe = ".exe";
#else
constexpr char kPathSep = ':';
constexpr const char* kExe = "";
#endif

std::string find_on_path(const std::string& name) {
    std::string path = env("PATH");
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find(kPathSep, start);
        if (end == std::string::npos) end = path.size();
        if (end > start) {
            fs::path candidate = fs::path(path.substr(start, end - start)) / (name + kExe);
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec)) return candidate.string();
        }
        start = end + 1;
    }
    return {};
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

#ifdef _WIN32
// Quotes one argument the way CommandLineToArgvW and the C runtime read it back.
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            backslashes++;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += c;
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

HANDLE start(const std::vector<std::string>& argv) {
    std::wstring cmd;
    for (std::size_t i = 0; i < argv.size(); i++) {
        if (i) cmd += L' ';
        cmd += quote_arg(widen(argv[i]));
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring app = widen(argv[0]);
    if (!CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

int finish(HANDLE h) {
    WaitForSingleObject(h, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return static_cast<int>(code);
}
#else
pid_t start(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid;
    if (posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ) != 0) return -1;
    return pid;
}

int finish(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}
#endif

}  // namespace

bool find_toolchain(Toolchain& tc, std::string& error) {
    tc.clang = env("CALX_CC");
    if (tc.clang.empty()) tc.clang = find_on_path("clang");
    if (tc.clang.empty()) {
        error = "can't find clang; put it on PATH or set CALX_CC";
        return false;
    }
    std::error_code ec;
    fs::path clang_path = fs::absolute(tc.clang, ec);
    if (!fs::is_regular_file(clang_path, ec)) {
        error = "clang isn't at " + tc.clang;
        return false;
    }
    tc.clang = clang_path.string();
    fs::path dir = clang_path.parent_path();
#ifdef _WIN32
    tc.lld = fs::is_regular_file(dir / "lld-link.exe", ec) || !find_on_path("lld-link").empty();
#else
    tc.lld = fs::is_regular_file(dir / "ld.lld", ec) || !find_on_path("ld.lld").empty();
#endif
    auto size = fs::file_size(clang_path, ec);
    auto time = fs::last_write_time(clang_path, ec).time_since_epoch().count();
    tc.identity = tc.clang + "|" + std::to_string(size) + "|" + std::to_string(time);

    tc.home = env("CALX_HOME");
#ifdef CALX_DEFAULT_HOME
    if (tc.home.empty()) tc.home = CALX_DEFAULT_HOME;
#endif
    if (!fs::is_regular_file(fs::path(tc.home) / "runtime" / "calx_rt.h", ec)) {
        error = "can't find the Calx runtime under " + tc.home + "; set CALX_HOME";
        return false;
    }

    tc.cache = env("CALX_CACHE");
    if (tc.cache.empty()) {
#ifdef _WIN32
        std::string base = env("LOCALAPPDATA");
        tc.cache = (fs::path(base.empty() ? env("TEMP") : base) / "calx" / "cache").string();
#else
        std::string base = env("XDG_CACHE_HOME");
        tc.cache = base.empty() ? (fs::path(env("HOME")) / ".cache" / "calx").string()
                                : (fs::path(base) / "calx").string();
#endif
    }
    fs::create_directories(fs::path(tc.cache) / "obj", ec);
    fs::create_directories(fs::path(tc.cache) / "bin", ec);
    return true;
}

std::string hash_hex(const std::vector<std::string_view>& parts) {
    // Two 64-bit FNV-1a lanes with different bases, over each part and its length.
    std::uint64_t a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
    auto mix = [&](unsigned char c) {
        a = (a ^ c) * 0x100000001b3ull;
        b = (b ^ c) * 0x100000001b3ull;
        b ^= b >> 29;
    };
    for (std::string_view p : parts) {
        for (unsigned char c : p) mix(c);
        std::uint64_t n = p.size();
        for (int i = 0; i < 8; i++) mix(static_cast<unsigned char>(n >> (8 * i)));
    }
    char buf[33];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b));
    return buf;
}

int run_process(const std::vector<std::string>& argv) {
    auto h = start(argv);
#ifdef _WIN32
    if (!h) return -1;
#else
    if (h < 0) return -1;
#endif
    return finish(h);
}

int run_parallel(const std::vector<std::vector<std::string>>& commands) {
#ifdef _WIN32
    std::vector<HANDLE> running;
#else
    std::vector<pid_t> running;
#endif
    int result = 0;
    for (const auto& c : commands) {
        auto h = start(c);
#ifdef _WIN32
        if (!h) {
#else
        if (h < 0) {
#endif
            result = -1;
            continue;
        }
        running.push_back(h);
    }
    for (auto h : running) {
        int r = finish(h);
        if (r != 0 && result == 0) result = r;
    }
    return result;
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool write_file_atomic(const std::string& path, const std::string& data) {
    std::string temp = path + ".tmp" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream out(temp, std::ios::binary);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

BuildResult build_program(const Toolchain& tc, const std::string& c_source, BuildMode mode, bool verbose) {
    BuildResult r;
    fs::path runtime = fs::path(tc.home) / "runtime";
    std::string rt_h, rt_c;
    if (!read_file((runtime / "calx_rt.h").string(), rt_h) || !read_file((runtime / "calx_rt.c").string(), rt_c)) {
        std::fprintf(stderr, "calx: can't read the runtime in %s\n", runtime.string().c_str());
        return r;
    }
    // Java's semantics hold in every mode: wrapping, no contraction (D63, CALX_SPEC.md).
    std::vector<std::string> flags = {"-std=gnu17", "-fwrapv", "-ffp-contract=off", "-fno-math-errno", "-w"};
    if (mode == BuildMode::debug) {
        flags.push_back("-O0");
    } else {
        flags.push_back("-O2");
        flags.push_back("-march=x86-64-v3");
    }
    std::string flag_text;
    for (const std::string& f : flags) flag_text += f + " ";

    std::string obj_dir = (fs::path(tc.cache) / "obj").string();
    std::string rt_key = hash_hex({"runtime", rt_h, rt_c, flag_text, tc.identity});
    std::string prog_key = hash_hex({"program", c_source, rt_h, flag_text, tc.identity});
    std::string rt_obj = (fs::path(obj_dir) / (rt_key + ".o")).string();
    std::string prog_obj = (fs::path(obj_dir) / (prog_key + ".o")).string();

    std::vector<std::string> link_flags;
    if (tc.lld) link_flags.push_back("-fuse-ld=lld");
#ifndef _WIN32
    link_flags.push_back("-lm");
#endif
    std::string link_text;
    for (const std::string& f : link_flags) link_text += f + " ";
    std::string exe_key = hash_hex({"exe", rt_key, prog_key, link_text, tc.identity});
    r.exe = (fs::path(tc.cache) / "bin" / (exe_key + kExe)).string();
    if (file_exists(r.exe)) {
        r.ok = true;
        r.reused = true;
        return r;
    }

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::vector<std::string>> compiles;
    std::vector<std::pair<std::string, std::string>> renames;
    auto compile = [&](const std::string& src_path, const std::string& obj, const std::vector<std::string>& extra) {
        std::string temp = obj + ".tmp" + std::to_string(compiles.size()) +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::vector<std::string> cmd = {tc.clang, "-c"};
        cmd.insert(cmd.end(), flags.begin(), flags.end());
        cmd.insert(cmd.end(), extra.begin(), extra.end());
        cmd.push_back("-I" + runtime.string());
        cmd.push_back(src_path);
        cmd.push_back("-o");
        cmd.push_back(temp);
        compiles.push_back(cmd);
        renames.push_back({temp, obj});
    };
    if (!file_exists(rt_obj)) compile((runtime / "calx_rt.c").string(), rt_obj, {"-D_CRT_SECURE_NO_WARNINGS"});
    std::string c_path;
    if (!file_exists(prog_obj)) {
        c_path = (fs::path(obj_dir) / (prog_key + ".c")).string();
        if (!write_file_atomic(c_path, c_source)) {
            std::fprintf(stderr, "calx: can't write %s\n", c_path.c_str());
            return r;
        }
        compile(c_path, prog_obj, {});
    }
    if (verbose) {
        for (const auto& c : compiles) {
            for (const std::string& a : c) std::fprintf(stderr, "%s ", a.c_str());
            std::fputc('\n', stderr);
        }
    }
    if (!compiles.empty()) {
        int status = run_parallel(compiles);
        std::error_code ec;
        if (status != 0) {
            for (auto& [temp, obj] : renames) fs::remove(temp, ec);
            std::fprintf(stderr, "calx: clang failed (status %d); this is a bug in the Calx compiler\n", status);
            return r;
        }
        for (auto& [temp, obj] : renames) {
            fs::rename(temp, obj, ec);
            if (ec && !file_exists(obj)) {
                std::fprintf(stderr, "calx: can't write %s\n", obj.c_str());
                return r;
            }
        }
        if (!c_path.empty()) fs::remove(c_path, ec);
    }
    r.compile_ms = ms_since(t0);

    auto t1 = std::chrono::steady_clock::now();
    std::string temp_exe = r.exe + ".tmp" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + kExe;
    std::vector<std::string> link = {tc.clang, prog_obj, rt_obj, "-o", temp_exe};
    link.insert(link.end(), link_flags.begin(), link_flags.end());
    if (verbose) {
        for (const std::string& a : link) std::fprintf(stderr, "%s ", a.c_str());
        std::fputc('\n', stderr);
    }
    if (run_process(link) != 0) {
        std::fprintf(stderr, "calx: linking failed\n");
        return r;
    }
    std::error_code ec;
    fs::rename(temp_exe, r.exe, ec);
    if (ec && !file_exists(r.exe)) {
        std::fprintf(stderr, "calx: can't write %s\n", r.exe.c_str());
        return r;
    }
#ifdef _WIN32
    // The linker's side files aren't needed.
    for (const char* ext : {".lib", ".exp", ".pdb"}) {
        fs::path side = fs::path(temp_exe).replace_extension(ext);
        fs::remove(side, ec);
    }
#endif
    r.link_ms = ms_since(t1);
    r.ok = true;
    return r;
}

}  // namespace calx
