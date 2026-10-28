#include "toolchain.h"

#include <algorithm>
#include <atomic>
#include <cctype>
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
constexpr const char* kLib = ".dll";
#else
constexpr char kPathSep = ':';
constexpr const char* kExe = "";
constexpr const char* kLib = ".so";
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

// Where a program's standard streams go: files, or the compiler's own when null. Errors go with
// the output when `errors` is null and `output` isn't.
struct Redirect {
    const std::string* input = nullptr;
    const std::string* output = nullptr;
    const std::string* errors = nullptr;
};

HANDLE open_for(const std::string& path, bool write) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    return CreateFileW(widen(path).c_str(), write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, &sa,
                       write ? CREATE_ALWAYS : OPEN_EXISTING, write ? FILE_ATTRIBUTE_TEMPORARY : FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

// Starts a program, with its streams where `r` says.
HANDLE start(const std::vector<std::string>& argv, Redirect r = {}) {
    std::wstring cmd;
    for (std::size_t i = 0; i < argv.size(); i++) {
        if (i) cmd += L' ';
        cmd += quote_arg(widen(argv[i]));
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    HANDLE files[3] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    auto close_all = [&] {
        for (HANDLE f : files) if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    };
    if (r.input || r.output) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        if (r.input) {
            files[0] = open_for(*r.input, false);
            if (files[0] == INVALID_HANDLE_VALUE) return nullptr;
            si.hStdInput = files[0];
        }
        if (r.output) {
            files[1] = open_for(*r.output, true);
            if (files[1] == INVALID_HANDLE_VALUE) {
                close_all();
                return nullptr;
            }
            si.hStdOutput = files[1];
            si.hStdError = files[1];
        }
        if (r.errors) {
            files[2] = open_for(*r.errors, true);
            if (files[2] == INVALID_HANDLE_VALUE) {
                close_all();
                return nullptr;
            }
            si.hStdError = files[2];
        }
    }
    PROCESS_INFORMATION pi{};
    std::wstring app = widen(argv[0]);
    BOOL ok = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    close_all();
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
// Where a program's standard streams go: files, or the compiler's own when null. Errors go with
// the output when `errors` is null and `output` isn't.
struct Redirect {
    const std::string* input = nullptr;
    const std::string* output = nullptr;
    const std::string* errors = nullptr;
};

// Starts a program, with its streams where `r` says.
pid_t start(const std::vector<std::string>& argv, Redirect r = {}) {
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (r.input) posix_spawn_file_actions_addopen(&actions, 0, r.input->c_str(), O_RDONLY, 0);
    if (r.output) {
        posix_spawn_file_actions_addopen(&actions, 1, r.output->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (r.errors) {
            posix_spawn_file_actions_addopen(&actions, 2, r.errors->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        } else {
            posix_spawn_file_actions_adddup2(&actions, 1, 2);
        }
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
    Redirect r;
    r.output = &output;
    auto h = start(argv, r);
#ifdef _WIN32
    if (!h) return -1;
#else
    if (h < 0) return -1;
#endif
    return finish(h);
}

int run_redirected(const std::vector<std::string>& argv, const std::string& input, const std::string& output,
                   const std::string& errors) {
    Redirect r{&input, &output, &errors};
    auto h = start(argv, r);
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
}  // namespace

const std::chrono::minutes kCacheGrace{10};

namespace {

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
    // Another build may have just reused an entry it's about to link, so nothing used in the last
    // few minutes goes, even past the limits: a test suite's builds side by side use more.
    fs::file_time_type recent = fs::file_time_type::clock::now() - kCacheGrace;
    for (std::size_t i = 0; i < entries.size(); i++) {
        kept += entries[i].second.bytes;
        if (i > 0 && (i >= keep || kept > budget) && entries[i].second.newest < recent) old.push_back(entries[i].first);
    }
    if (old.empty()) return;
    std::sort(old.begin(), old.end());
    for (const auto& [hash, path] : files) {
        if (std::binary_search(old.begin(), old.end(), hash)) fs::remove(path, ec);
    }
}

// The compiler's own executable.
fs::path self_exe() {
#ifdef _WIN32
    std::wstring w(1024, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, w.data(), static_cast<DWORD>(w.size()));
        if (n == 0) return {};
        if (n < w.size()) {
            w.resize(n);
            return fs::path(w);
        }
        w.resize(w.size() * 2);
    }
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

}  // namespace

std::string compiler_hash(const Toolchain& tc) {
    fs::path exe = self_exe();
    std::error_code ec;
    auto size = fs::file_size(exe, ec);
    if (ec) return {};
    auto time = fs::last_write_time(exe, ec).time_since_epoch().count();
    if (ec) return {};
    std::string id = exe.string() + "|" + std::to_string(size) + "|" + std::to_string(time);
    fs::path saved = fs::path(tc.cache) / "abi" / (hash_hex({"compiler", id}) + ".txt");
    std::string self;
    if (!read_file(saved.string(), self) || self.size() != 32) {
        std::string bytes;
        if (!read_file(exe.string(), bytes)) return {};
        self = hash_hex({"compiler-bytes", bytes});
        fs::create_directories(saved.parent_path(), ec);
        write_file_atomic(saved.string(), self);
    }
    return self;
}

namespace {

// The runtime as a library sees it (D123): every function calx_rt.h declares with `CX_FN`, in
// order, and a hash of the compiler and the header, which a library and the program that opens it
// must share, since the compiler decides how Calx values are laid out and passed. The compiler's
// part is a hash of its own bytes, kept in the cache under its size and time, so it's read once.
struct RuntimeApi {
    std::vector<std::string> fns;
    std::uint64_t abi = 0;
};

bool runtime_api(const Toolchain& tc, const std::string& header, RuntimeApi& api) {
    for (std::size_t at = header.find("CX_FN("); at != std::string::npos; at = header.find("CX_FN(", at + 1)) {
        // A declaration starts its line, after at most an attribute: not the macro's own
        // definition, nor a comment that names it.
        std::size_t line = header.rfind('\n', at);
        line = line == std::string::npos ? 0 : line + 1;
        std::string_view before(header.data() + line, at - line);
        if (!before.empty() && before != "CX_COLD " && before != "__attribute__((cold)) ") continue;
        // `CX_FN(ret, name, (params))`: the name is after the first comma.
        std::size_t comma = header.find(',', at);
        if (comma == std::string::npos) return false;
        std::size_t b = comma + 1;
        while (b < header.size() && header[b] == ' ') b++;
        std::size_t e = b;
        while (e < header.size() && (std::isalnum(static_cast<unsigned char>(header[e])) || header[e] == '_')) e++;
        if (e == b) return false;
        api.fns.push_back(header.substr(b, e - b));
    }
    std::string self = compiler_hash(tc);
    if (self.empty()) return false;
    std::string h = hash_hex({"abi", self, header});
    api.abi = std::stoull(h.substr(0, 16), nullptr, 16);
    return true;
}

std::string hex64(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%016llxull", static_cast<unsigned long long>(v));
    return buf;
}

// A program that opens libraries holds the address of each runtime function, in the header's
// order, after the empty table every table starts at.
std::string runtime_table_c(const RuntimeApi& api) {
    std::string s = "/* The runtime functions a library takes from this program (D123). */\n#include \"calx_rt.h\"\n\n"
                    "void* const cx_runtime_fns[] = {\n    (void*)cx_table_empty,\n";
    for (const std::string& f : api.fns) s += "    (void*)&" + f + ",\n";
    s += "};\nconst cx_usize cx_runtime_count = " + std::to_string(api.fns.size()) + ";\nconst cx_u64 cx_runtime_abi = " +
         hex64(api.abi) + ";\n";
    return s;
}

// A library's pointers to those functions, which `attach` fills in, and `cx_library`, the one
// symbol it exports, which says what it was built with and lists what it exports.
std::string library_attach_c(const RuntimeApi& api) {
    std::string s = "/* This library's pointers to the runtime functions of the program that opens it (D123). */\n"
                    "#include \"calx_rt.h\"\n\nconst cx_u8* cx_table_empty_at;\n";
    for (const std::string& f : api.fns) s += "__typeof__(" + f + ") " + f + ";\n";
    s += "\nstatic void attach(void* const* f) {\n    cx_table_empty_at = (const cx_u8*)f[0];\n";
    for (std::size_t i = 0; i < api.fns.size(); i++) {
        s += "    " + api.fns[i] + " = (__typeof__(" + api.fns[i] + "))f[" + std::to_string(i + 1) + "];\n";
    }
    s += "}\n\nconst cx_lib_export* cx_lib_exports(cx_usize* count);\n\n"
         "#ifdef _WIN32\n__declspec(dllexport)\n#else\n__attribute__((visibility(\"default\")))\n#endif\n"
         "const cx_lib_info cx_library = {" + hex64(api.abi) + ", " + std::to_string(api.fns.size()) +
         ", attach, cx_lib_exports};\n";
    return s;
}

}  // namespace

const char* library_suffix() { return kLib; }

bool place_build(const std::string& built, const std::string& out, std::string& error) {
    std::error_code ec;
    // The output's folder is made if it isn't there, as `-o build/app` expects.
    fs::path parent = fs::path(out).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    fs::copy_file(built, out, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "can't write " + out + ": " + ec.message();
        return false;
    }
#ifdef _WIN32
    // The image names its PDB by the path it was linked with, in the cache, which a later build
    // can trim. So the PDB goes beside it as `out.pdb`, and the image names that instead: the
    // name alone, which debuggers and dbghelp look for beside the image.
    std::string image;
    if (!read_file(out, image)) {
        error = "can't read " + out;
        return false;
    }
    auto u16 = [&](std::size_t at) -> std::uint32_t {
        return at + 2 <= image.size() ? static_cast<unsigned char>(image[at]) | static_cast<unsigned char>(image[at + 1]) << 8 : 0;
    };
    auto u32 = [&](std::size_t at) -> std::uint32_t { return u16(at) | u16(at + 2) << 16; };
    std::size_t pe = u32(0x3C);
    if (image.compare(pe, 4, std::string("PE\0\0", 4)) != 0) return true;
    std::size_t opt = pe + 24;
    std::size_t dirs = opt + (u16(opt) == 0x20B ? 112 : 96);
    std::uint32_t debug_rva = u32(dirs + 6 * 8), debug_size = u32(dirs + 6 * 8 + 4);
    std::size_t sections = opt + u16(pe + 20), count = u16(pe + 6);
    std::size_t debug = 0;
    for (std::size_t i = 0; i < count; i++) {
        std::size_t s = sections + 40 * i;
        std::uint32_t vsize = u32(s + 8), va = u32(s + 12), rsize = u32(s + 16), raw = u32(s + 20);
        if (debug_rva >= va && debug_rva < va + std::max(vsize, rsize)) debug = debug_rva - va + raw;
    }
    if (!debug) return true;
    for (std::size_t e = debug; e + 28 <= debug + debug_size && e + 28 <= image.size(); e += 28) {
        std::uint32_t type = u32(e + 12), size = u32(e + 16), at = u32(e + 24);
        if (type != 2 || at + size > image.size() || size < 25 || image.compare(at, 4, "RSDS") != 0) continue;
        std::size_t name_at = at + 24, room = size - 24;
        std::string pdb = image.substr(name_at, image.find('\0', name_at) - name_at);
        fs::path to = fs::path(out).replace_extension(".pdb");
        std::string name = to.filename().string();
        if (name.size() + 1 > room || !file_exists(pdb)) return true;
        fs::copy_file(pdb, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "can't write " + to.string() + ": " + ec.message();
            return false;
        }
        image.replace(name_at, room, name + std::string(room - name.size(), '\0'));
        if (!write_file_atomic(out, image)) {
            error = "can't write " + out;
            return false;
        }
        return true;
    }
#endif
    return true;
}

BuildResult build_program(const Toolchain& tc, const std::vector<std::string>& c_sources_in, BuildMode mode, BuildKind kind,
                          bool verbose) {
    BuildResult r;
    bool library = kind == BuildKind::library;
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
    // A library takes the runtime's functions from the program that opens it, and a program that
    // opens libraries has every one of them to give (D123).
    std::vector<std::string> c_sources = c_sources_in;
    bool opens = false;
    for (const std::string& c : c_sources) opens = opens || (!library && c.find("cx_lib_") != std::string::npos);
    if (library || opens) {
        RuntimeApi api;
        if (!runtime_api(tc, rt_h, api)) {
            std::fprintf(stderr, "calx: can't read the runtime's functions in %s\n", runtime.string().c_str());
            return r;
        }
        c_sources.push_back(library ? library_attach_c(api) : runtime_table_c(api));
    }
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
    if (library) {
        flags.push_back("-DCX_LIBRARY");
#ifndef _WIN32
        // Only `cx_library` is for the system to find.
        flags.push_back("-fPIC");
        flags.push_back("-fvisibility=hidden");
#endif
    }
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
    // the tables that change the case of text (D117), the hashes, crypto, zlib, lz4 and HTTP (D118),
    // threads and their queues (D121), and files.
    auto calls = [&](std::initializer_list<const char*> names) {
        if (opens) return true;
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
    if (parts_ok && opens) parts_ok = part("calx_lib.c");
    if (parts_ok && calls({"cx_net_"})) parts_ok = part("calx_net.c");
    if (parts_ok && calls({"cx_jt_"})) parts_ok = part("calx_jtable.c");
    if (parts_ok && calls({"cx_oh_"})) parts_ok = part("calx_ohtable.c");
    if (parts_ok && calls({"cx_case_"})) parts_ok = part("calx_case.c");
    if (parts_ok && calls({"cx_digest_"})) parts_ok = part("calx_digest.c");
    if (parts_ok && calls({"cx_crypto_", "cx_cfb8_", "cx_rsa_"})) parts_ok = part("calx_crypto.c");
    if (parts_ok && calls({"cx_zlib_", "cx_inflate", "cx_deflate"})) parts_ok = part("calx_zlib.c");
    if (parts_ok && calls({"cx_http_"})) parts_ok = part("calx_http.c");
    if (parts_ok && calls({"cx_lz4_"})) parts_ok = part("calx_lz4.c");
    if (parts_ok && calls({"cx_fs_"})) parts_ok = part("calx_fs.c");
    if (parts_ok && calls({"cx_queue_", "cx_thread_", "cx_Sender", "cx_Receiver", "cx_Thread"})) parts_ok = part("calx_thread.c");
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
    if (library) {
        link_flags.push_back("-shared");
#ifdef _WIN32
        link_flags.push_back("-Wl,/noimplib");  // nothing links against it: programs open it
#endif
    }
#ifdef _WIN32
    // The main thread's stack is 8 MiB, as Linux and macOS give it and as the runtime gives every
    // thread it starts, so a program recurses as deep on every system (D135). Windows' is 1 MiB
    // unless the program says. It's address space, taken as memory only as the stack grows.
    if (!library) link_flags.push_back("-Wl,/STACK:8388608");
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
    const char* suffix = library ? kLib : kExe;
    r.exe = (fs::path(tc.cache) / "bin" / (exe_key + suffix)).string();
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
    if (!library) {  // a library links none of the runtime
        if (file_exists(rt_obj)) touch(rt_obj);
        else compile((runtime / "calx_rt.c").string(), rt_obj, {"-D_CRT_SECURE_NO_WARNINGS"});
        if (file_exists(alloc_obj)) touch(alloc_obj);
        else compile((runtime / "calx_alloc.c").string(), alloc_obj, {"-D_CRT_SECURE_NO_WARNINGS"});
        for (const auto& [file, obj] : parts) {
            if (file_exists(obj)) touch(obj);
            else compile((runtime / file).string(), obj, {"-D_CRT_SECURE_NO_WARNINGS"});
        }
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
    std::string temp_exe = (fs::path(tc.cache) / "bin" / (exe_key + "." + stamp + suffix)).string();
    std::vector<std::string> inputs = objs;
    if (!library) {
        inputs.push_back(rt_obj);
        inputs.push_back(alloc_obj);
        for (const auto& [file, obj] : parts) inputs.push_back(obj);
    }
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
