#include "toolchain.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
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

// A name part no other build, in this process or another, uses at the same time: for the
// temporary files a build writes into the shared cache before renaming them.
std::string unique_stamp() {
    static std::atomic<unsigned long long> count{0};
#ifdef _WIN32
    unsigned long long pid = GetCurrentProcessId();
#else
    unsigned long long pid = static_cast<unsigned long long>(getpid());
#endif
    return std::to_string(pid) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(count++);
}

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

// Starts a program. With `output`, its standard output and error go to that file.
HANDLE start(const std::vector<std::string>& argv, const std::string* output = nullptr) {
    std::wstring cmd;
    for (std::size_t i = 0; i < argv.size(); i++) {
        if (i) cmd += L' ';
        cmd += quote_arg(widen(argv[i]));
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    HANDLE file = INVALID_HANDLE_VALUE;
    if (output) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        file = CreateFileW(widen(*output).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE) return nullptr;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = file;
        si.hStdError = file;
    }
    PROCESS_INFORMATION pi{};
    std::wstring app = widen(argv[0]);
    BOOL ok = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!ok) return nullptr;
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
// Starts a program. With `output`, its standard output and error go to that file.
pid_t start(const std::vector<std::string>& argv, const std::string* output = nullptr) {
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (output) {
        posix_spawn_file_actions_addopen(&actions, 1, output->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
    }
    pid_t pid;
    int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    return rc == 0 ? pid : -1;
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

std::string find_home(std::string& error) {
    std::string home = env("CALX_HOME");
#ifdef CALX_DEFAULT_HOME
    if (home.empty()) home = CALX_DEFAULT_HOME;
#endif
    std::error_code ec;
    if (!fs::is_regular_file(fs::path(home) / "runtime" / "calx_rt.h", ec)) {
        error = "can't find the Calx runtime under " + (home.empty() ? std::string("CALX_HOME") : home) + "; set CALX_HOME";
        return "";
    }
    return home;
}

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

    tc.home = find_home(error);
    if (tc.home.empty()) return false;

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

int run_to_file(const std::vector<std::string>& argv, const std::string& output) {
    auto h = start(argv, &output);
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
    std::string temp = path + ".tmp" + unique_stamp();
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

namespace {

// Reads the command lines `clang -###` prints, which quote every argument and escape `"`, `\`
// and `$` with a backslash.
std::vector<std::vector<std::string>> parse_commands(const std::string& text) {
    std::vector<std::vector<std::string>> commands;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + pos, end - pos);
        pos = end + 1;
        std::size_t i = line.find_first_not_of(' ');
        if (i == std::string_view::npos || line[i] != '"') continue;
        std::vector<std::string> args;
        while (i < line.size()) {
            if (line[i] == ' ' || line[i] == '\r') {
                i++;
                continue;
            }
            std::string arg;
            if (line[i] == '"') {
                i++;
                while (i < line.size() && line[i] != '"') {
                    if (line[i] == '\\' && i + 1 < line.size()) i++;
                    arg += line[i++];
                }
                i++;
            } else {
                while (i < line.size() && line[i] != ' ' && line[i] != '\r') arg += line[i++];
            }
            args.push_back(std::move(arg));
        }
        commands.push_back(std::move(args));
    }
    return commands;
}

// Replaces every occurrence of `from` in each argument with `to`, and returns how many there were.
int substitute(std::vector<std::string>& args, const std::string& from, const std::string& to) {
    int count = 0;
    for (std::string& a : args) {
        for (std::size_t at = a.find(from); at != std::string::npos; at = a.find(from, at + to.size())) {
            a.replace(at, from.size(), to);
            count++;
        }
    }
    return count;
}

const char* const kLinkInputs = "{calx-inputs}";  // stands for every object, however many
const char* const kLinkOutput = "{calx-output}";

// The environment clang reads to find the C runtime and the system libraries.
const char* const kLinkEnv[] = {"VCToolsInstallDir", "VCINSTALLDIR", "VSINSTALLDIR", "WindowsSdkDir",
                                "WindowsSDKVersion", "UniversalCRTSdkDir", "UCRTVersion", "LIB",
                                "LIBRARY_PATH", "SDKROOT"};

void print_command(const std::vector<std::string>& argv) {
    for (const std::string& a : argv) std::fprintf(stderr, "%s ", a.c_str());
    std::fputc('\n', stderr);
}

void print_file(const std::string& path) {
    std::string text;
    if (read_file(path, text)) std::fwrite(text.data(), 1, text.size(), stderr);
}

// Links `inputs` into `output`. For a link, clang's driver only works out the linker's command
// line, which costs about 18 ms of starting clang each time. So the first link asks it once with
// `-###`, and caches the answer as a template, and later links run the linker straight from it
// (D64). If a cached command fails, say because Visual Studio moved, the driver works it out again.
bool link_program(const Toolchain& tc, const std::vector<std::string>& inputs, const std::string& output,
                  const std::vector<std::string>& link_flags, bool verbose) {
    std::vector<std::string> driver = {tc.clang};
    driver.insert(driver.end(), inputs.begin(), inputs.end());
    driver.push_back("-o");
    driver.push_back(output);
    driver.insert(driver.end(), link_flags.begin(), link_flags.end());

    std::vector<std::string> key_text = {"link", tc.identity};
    key_text.insert(key_text.end(), link_flags.begin(), link_flags.end());
    for (const char* name : kLinkEnv) key_text.push_back(std::string(name) + "=" + env(name));
    std::vector<std::string_view> key_parts(key_text.begin(), key_text.end());
    std::string template_path = (fs::path(tc.cache) / "link" / (hash_hex(key_parts) + ".txt")).string();
    std::string log = output + ".log";
    std::error_code ec;

    auto fill = [&](const std::vector<std::string>& tmpl) {
        std::vector<std::string> args;
        for (const std::string& a : tmpl) {
            if (a == kLinkInputs) args.insert(args.end(), inputs.begin(), inputs.end());
            else args.push_back(a);
        }
        substitute(args, kLinkOutput, output);
        return args;
    };

    std::string saved;
    if (read_file(template_path, saved)) {
        std::vector<std::string> args;
        std::size_t pos = 0;
        while (pos < saved.size()) {
            std::size_t end = saved.find('\n', pos);
            if (end == std::string::npos) end = saved.size();
            args.push_back(saved.substr(pos, end - pos));
            pos = end + 1;
        }
        args = fill(std::move(args));
        if (verbose) print_command(args);
        if (!args.empty() && run_to_file(args, log) == 0) {
            if (verbose) print_file(log);
            fs::remove(log, ec);
            return true;
        }
        fs::remove(log, ec);
        fs::remove(template_path, ec);
    }

    // Ask the driver for the linker's command line.
    std::vector<std::string> ask = driver;
    ask.push_back("-###");
    std::vector<std::vector<std::string>> commands;
    if (run_to_file(ask, log) == 0) {
        std::string text;
        if (read_file(log, text)) commands = parse_commands(text);
    }
    fs::remove(log, ec);
    if (commands.size() == 1 && !commands[0].empty()) {
        std::vector<std::string> args = commands[0];
#ifdef _WIN32
        // `-###` names the linker without `.exe`, which CreateProcess needs.
        if (!file_exists(args[0]) && file_exists(args[0] + ".exe")) args[0] += ".exe";
#endif
        if (verbose) print_command(args);
        int status = run_process(args);
        if (status > 0) return false;
        if (status < 0) {
            if (verbose) print_command(driver);
            return run_process(driver) == 0;
        }
        // Cached only if the inputs appear once each, whole and in order, so one placeholder can
        // stand for them, and the template rebuilds this command.
        std::vector<std::string> tmpl;
        auto first = std::find(args.begin(), args.end(), inputs.empty() ? std::string() : inputs[0]);
        bool exact = !inputs.empty() && first != args.end() &&
                     static_cast<std::size_t>(args.end() - first) >= inputs.size() &&
                     std::equal(inputs.begin(), inputs.end(), first);
        if (exact) {
            tmpl.assign(args.begin(), first);
            tmpl.push_back(kLinkInputs);
            tmpl.insert(tmpl.end(), first + static_cast<std::ptrdiff_t>(inputs.size()), args.end());
            for (const std::string& in : inputs) exact = exact && std::count(tmpl.begin(), tmpl.end(), in) == 0;
            exact = exact && substitute(tmpl, output, kLinkOutput) >= 1;
        }
        if (exact && fill(tmpl) == args) {
            std::string text;
            for (const std::string& a : tmpl) text += a + "\n";
            text.pop_back();
            fs::create_directories(fs::path(template_path).parent_path(), ec);
            write_file_atomic(template_path, text);
        }
        return true;
    }
    if (verbose) print_command(driver);
    return run_process(driver) == 0;
}

// What the cache keeps (D64). On Windows a debug program and its PDB take under 1 MB, and a
// release one about 7 MB, mostly the static C runtime's debug info.
constexpr std::size_t kKeepPrograms = 64;
constexpr std::uintmax_t kProgramBytes = 256u << 20;
constexpr std::size_t kKeepObjects = 1024;
constexpr std::uintmax_t kObjectBytes = 256u << 20;

// Marks a cached file as just used, for trim.
void touch(const std::string& path) {
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
}

// Keeps the most recently used entries of a cache folder, up to `keep` of them and `budget`
// bytes, and deletes the rest. The newest entry always stays. An entry is every file whose name
// starts with the same 32-digit hash, such as a program and its PDB.
void trim(const fs::path& dir, std::size_t keep, std::uintmax_t budget) {
    struct Entry {
        // Not {}: libstdc++'s file clock counts from 2174, so {} is later than any real time.
        fs::file_time_type newest = fs::file_time_type::min();
        std::uintmax_t bytes = 0;
    };
    std::error_code ec;
    std::unordered_map<std::string, Entry> by_hash;
    std::vector<std::pair<std::string, fs::path>> files;
    std::uintmax_t total = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        std::string name = e.path().filename().string();
        if (name.size() < 32 || !e.is_regular_file(ec)) continue;
        std::string hash = name.substr(0, 32);
        fs::file_time_type t = e.last_write_time(ec);
        std::uintmax_t size = e.file_size(ec);
        files.push_back({hash, e.path()});
        Entry& entry = by_hash[hash];
        if (t > entry.newest) entry.newest = t;
        entry.bytes += size;
        total += size;
    }
    if (by_hash.size() <= keep && total <= budget) return;
    std::vector<std::pair<std::string, Entry>> entries(by_hash.begin(), by_hash.end());
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.second.newest > b.second.newest; });
    std::vector<std::string> old;
    std::uintmax_t kept = 0;
    for (std::size_t i = 0; i < entries.size(); i++) {
        kept += entries[i].second.bytes;
        if (i > 0 && (i >= keep || kept > budget)) old.push_back(entries[i].first);
    }
    if (old.empty()) return;
    std::sort(old.begin(), old.end());
    for (const auto& [hash, path] : files) {
        if (std::binary_search(old.begin(), old.end(), hash)) fs::remove(path, ec);
    }
}

}  // namespace

BuildResult build_program(const Toolchain& tc, const std::vector<std::string>& c_sources, BuildMode mode, bool verbose) {
    BuildResult r;
    fs::path runtime = fs::path(tc.home) / "runtime";
    std::string rt_h, rt_c, sort_h, btree_h, heap_h;
    if (!read_file((runtime / "calx_rt.h").string(), rt_h) || !read_file((runtime / "calx_rt.c").string(), rt_c) ||
        !read_file((runtime / "calx_sort.h").string(), sort_h) || !read_file((runtime / "calx_btree.h").string(), btree_h) ||
        !read_file((runtime / "calx_heap.h").string(), heap_h)) {
        std::fprintf(stderr, "calx: can't read the runtime in %s\n", runtime.string().c_str());
        return r;
    }
    // Any file may include the sort's template, the sorted tables' (D112) or the queues' (D113), so
    // the keys cover them as they cover the header.
    rt_h += sort_h;
    rt_h += btree_h;
    rt_h += heap_h;
    // Java's semantics hold in every mode: wrapping, no contraction (D63, CALX_SPEC.md).
    // Line tables in both modes, so debuggers and profilers show Calx lines through `#line`.
    // They don't change the generated code.
    std::vector<std::string> flags = {"-std=gnu17", "-fwrapv", "-ffp-contract=off", "-fno-math-errno", "-w",
                                      "-gline-tables-only"};
    if (mode == BuildMode::debug) {
        flags.push_back("-O0");
        flags.push_back("-DCX_DEBUG");  // the runtime checks that everything allocated is freed
#ifdef _WIN32
        // The DLL C runtime. Linking the static one copies its debug info into every PDB, which
        // doubles the link time. Release builds link it statically, so they run anywhere.
        flags.push_back("-fms-runtime-lib=dll");
#endif
    } else {
        flags.push_back("-O3");
        flags.push_back("-march=x86-64-v3");
    }
    // Release builds compile each file to LLVM bitcode, and optimize the program as a whole when
    // it links (ThinLTO), so a small function inlines into every module that calls it, as it would
    // within one, and so do the runtime's. Only lld can; without it each file is optimized alone
    // (D99).
    bool lto = mode == BuildMode::release && tc.lld;
    if (lto) flags.push_back("-flto=thin");
    std::string flag_text;
    for (const std::string& f : flags) flag_text += f + " ";

    std::string obj_dir = (fs::path(tc.cache) / "obj").string();
    std::string rt_key = hash_hex({"runtime", rt_h, rt_c, flag_text, tc.identity});
    std::string rt_obj = (fs::path(obj_dir) / (rt_key + ".o")).string();
    std::vector<std::string> keys, objs;
    std::string all_keys = rt_key;
    // Parts of the runtime that only some programs call are files of their own, compiled and
    // linked only into those: networking, so the others don't load the socket libraries when they
    // start, the sorts of integers (D96), which take clang a second to optimize, Java's tables (D105),
    // the tables that change the case of text (D117), and the hashes, crypto, zlib, lz4 and HTTP (D118).
    auto calls = [&](std::initializer_list<const char*> names) {
        for (const std::string& c : c_sources) {
            for (const char* n : names) {
                if (c.find(n) != std::string::npos) return true;
            }
        }
        return false;
    };
    std::vector<std::pair<std::string, std::string>> parts;  // each file and its object
    auto part = [&](const char* file) {
        std::string text;
        if (!read_file((runtime / file).string(), text)) return false;
        std::string key = hash_hex({"runtime-part", file, rt_h, text, flag_text, tc.identity});
        parts.push_back({file, (fs::path(obj_dir) / (key + ".o")).string()});
        all_keys += " " + key;
        return true;
    };
    // Stack traces are in every program, since any can panic, in a file of their own since they
    // read debug information, which the rest of the runtime never does.
    bool parts_ok = part("calx_trace.c");
    if (parts_ok && calls({"cx_net_"})) parts_ok = part("calx_net.c");
    if (parts_ok && calls({"cx_jt_"})) parts_ok = part("calx_jtable.c");
    if (parts_ok && calls({"cx_oh_"})) parts_ok = part("calx_ohtable.c");
    if (parts_ok && calls({"cx_case_"})) parts_ok = part("calx_case.c");
    if (parts_ok && calls({"cx_digest_"})) parts_ok = part("calx_digest.c");
    if (parts_ok && calls({"cx_crypto_", "cx_cfb8_", "cx_rsa_"})) parts_ok = part("calx_crypto.c");
    if (parts_ok && calls({"cx_zlib_", "cx_inflate", "cx_deflate"})) parts_ok = part("calx_zlib.c");
    if (parts_ok && calls({"cx_http_"})) parts_ok = part("calx_http.c");
    if (parts_ok && calls({"cx_lz4_"})) parts_ok = part("calx_lz4.c");
    if (parts_ok && calls({"cx_sort_i8(", "cx_sort_i16(", "cx_sort_i32(", "cx_sort_i64(", "cx_sort_u8(", "cx_sort_u16(",
                           "cx_sort_u32(", "cx_sort_u64(", "cx_sort_usize(", "cx_sort_char(", "cx_sort_rune(", "cx_sort_bool("})) {
        parts_ok = part("calx_sort.c");
    }
    if (!parts_ok) {
        std::fprintf(stderr, "calx: can't read the runtime in %s\n", runtime.string().c_str());
        return r;
    }
    // The allocator is a file of its own, linked into every program (D95).
    std::string alloc_c;
    if (!read_file((runtime / "calx_alloc.c").string(), alloc_c)) {
        std::fprintf(stderr, "calx: can't read the runtime in %s\n", runtime.string().c_str());
        return r;
    }
    std::string alloc_key = hash_hex({"runtime-alloc", rt_h, alloc_c, flag_text, tc.identity});
    std::string alloc_obj = (fs::path(obj_dir) / (alloc_key + ".o")).string();
    all_keys += " " + alloc_key;
    for (const std::string& c : c_sources) {
        keys.push_back(hash_hex({"module", c, rt_h, flag_text, tc.identity}));
        objs.push_back((fs::path(obj_dir) / (keys.back() + ".o")).string());
        all_keys += " " + keys.back();
    }

    std::vector<std::string> link_flags;
    if (tc.lld) link_flags.push_back("-fuse-ld=lld");
#ifdef _WIN32
    link_flags.push_back("-g");  // writes the PDB that holds the line tables
    // The driver always names the static C runtime when linking; the objects name the DLL one.
    if (mode == BuildMode::debug) link_flags.push_back("-Wl,-nodefaultlib:libcmt");
#else
    link_flags.push_back("-lm");
#endif
    if (lto) {
        // lld keeps each file's machine code in a cache of its own, so a link after an edit
        // compiles again only the files the edit could change. It prunes the cache itself.
        std::string lto_cache = (fs::path(tc.cache) / "lto").string();
        link_flags.push_back("-flto=thin");
#ifdef _WIN32
        // The driver doesn't pass the optimization level to lld-link, which would use -O2.
        for (const char* f : {"/opt:lldlto=3", "/lldltocachepolicy:cache_size_bytes=512m"}) {
            link_flags.push_back("-Xlinker");
            link_flags.push_back(f);
        }
        link_flags.push_back("-Xlinker");
        link_flags.push_back("/lldltocache:" + lto_cache);
#else
        link_flags.push_back("-O3");
        link_flags.push_back("-march=x86-64-v3");
        for (const std::string& f : {std::string("--thinlto-cache-policy=cache_size_bytes=512m"),
                                     "--thinlto-cache-dir=" + lto_cache}) {
            link_flags.push_back("-Xlinker");
            link_flags.push_back(f);
        }
#endif
    }
    std::string link_text;
    for (const std::string& f : link_flags) link_text += f + " ";
    std::string exe_key = hash_hex({"exe", all_keys, link_text, tc.identity});
    r.exe = (fs::path(tc.cache) / "bin" / (exe_key + kExe)).string();
    if (file_exists(r.exe)) {
        touch(r.exe);
        r.ok = true;
        r.reused = true;
        return r;
    }

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::vector<std::string>> compiles;
    std::vector<std::pair<std::string, std::string>> renames;
    auto compile = [&](const std::string& src_path, const std::string& obj, const std::vector<std::string>& extra) {
        std::string temp = obj + ".tmp" + unique_stamp();
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
    if (file_exists(rt_obj)) {
        touch(rt_obj);
    } else {
        compile((runtime / "calx_rt.c").string(), rt_obj, {"-D_CRT_SECURE_NO_WARNINGS"});
    }
    if (file_exists(alloc_obj)) touch(alloc_obj);
    else compile((runtime / "calx_alloc.c").string(), alloc_obj, {"-D_CRT_SECURE_NO_WARNINGS"});
    for (const auto& [file, obj] : parts) {
        if (file_exists(obj)) touch(obj);
        else compile((runtime / file).string(), obj, {"-D_CRT_SECURE_NO_WARNINGS"});
    }
    // Each build writes its C under a name of its own: two builds that need the same module, such
    // as a std one, would otherwise share the file, and one could delete it while the other's
    // clang is still to read it.
    std::string c_stamp = unique_stamp();
    std::vector<std::string> c_paths;
    for (std::size_t i = 0; i < c_sources.size(); i++) {
        if (file_exists(objs[i])) {
            touch(objs[i]);
            continue;
        }
        std::string c_path = (fs::path(obj_dir) / (keys[i] + "." + c_stamp + ".c")).string();
        if (!write_file_atomic(c_path, c_sources[i])) {
            std::fprintf(stderr, "calx: can't write %s\n", c_path.c_str());
            return r;
        }
        c_paths.push_back(c_path);
        compile(c_path, objs[i], {});
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
        for (const std::string& c_path : c_paths) fs::remove(c_path, ec);
    }
    r.compile_ms = ms_since(t0);

    auto t1 = std::chrono::steady_clock::now();
    // Linked under a unique name, then renamed, so two builds of one program can't collide. On
    // Windows the PDB keeps the unique name, which the executable records, so debuggers find it.
    std::string stamp = unique_stamp();
    std::string temp_exe = (fs::path(tc.cache) / "bin" / (exe_key + "." + stamp + kExe)).string();
    std::vector<std::string> inputs = objs;
    inputs.push_back(rt_obj);
    inputs.push_back(alloc_obj);
    for (const auto& [file, obj] : parts) inputs.push_back(obj);
    if (!link_program(tc, inputs, temp_exe, link_flags, verbose)) {
        std::fprintf(stderr, "calx: linking failed\n");
        return r;
    }
    std::error_code ec;
    fs::rename(temp_exe, r.exe, ec);
    if (ec && !file_exists(r.exe)) {
        std::fprintf(stderr, "calx: can't write %s\n", r.exe.c_str());
        return r;
    }
    r.link_ms = ms_since(t1);
    r.ok = true;
    trim(fs::path(tc.cache) / "bin", kKeepPrograms, kProgramBytes);
    trim(obj_dir, kKeepObjects, kObjectBytes);
    return r;
}

}  // namespace calx
