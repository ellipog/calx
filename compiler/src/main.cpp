// The `calx` command.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "emit.h"
#include "expand.h"
#include "format.h"
#include "manage.h"
#include "modules.h"
#include "parser.h"
#include "source.h"
#include "toolchain.h"
#include "update.h"
#include "doctor.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

using namespace calx;

namespace {

// `calx 0.2.0 (3f9c2a1b7d4e, 2026-11-01)` for a release or a nightly, which CI stamps with its
// commit and date, and `calx 0.2.0-dev (built from source)` otherwise.
void print_version() {
    if (CALX_COMMIT[0]) {
        std::printf("calx %s (%s, %s)\n", CALX_VERSION, CALX_COMMIT, CALX_DATE);
    } else {
        std::printf("calx %s (built from source)\n", CALX_VERSION);
    }
}

int usage() {
    std::fputs(
        "usage: calx <command> [options] <file.calx>\n"
        "       calx [options] <file.calx> [arguments]   run a program, giving it the arguments\n"
        "\n"
        "commands:\n"
        "  new       start a program: calx new hello makes hello/hello.calx, calx new tidy.calx a script\n"
        "  check     check a program without building it\n"
        "  build     build a program (-o names the output)\n"
        "  run       build a program and run it; arguments after -- go to the program\n"
        "  test      build a file's @test functions and run each, as many at once as there are\n"
        "            processors; names after the file pick the tests whose names hold one\n"
        "  eval      run code given as one argument, and print its last expression's value:\n"
        "            calx eval \"1 << 20\", or calx eval \"let x = 3; x * x\"\n"
        "  emit-c    print the C a program compiles to (-o names a file)\n"
        "  expand    print the code a program's attributes generate\n"
        "  fmt       format .calx files in place, or those under a folder (--check changes nothing,\n"
        "            --stdin formats standard input to standard output)\n"
        "  update    install the newest release (--check, --nightly, --stable, --to V, --rollback)\n"
        "  doctor    check that Calx can build and run programs, and say how to fix what's wrong\n"
        "  clean     remove the build cache, and say how much that freed\n"
        "  uninstall remove Calx, its build cache and its PATH entry (--yes doesn't ask)\n"
        "  completions SHELL\n"
        "            print completions for bash, zsh, fish or powershell\n"
        "  version   print the compiler's version\n"
        "\n"
        "options:\n"
        "  --release   optimize (the default is a debug build, which builds fastest)\n"
        "  --lib       build a library, which a program opens while it runs\n"
        "  -o PATH     where build and emit-c write\n"
        "  --timings   print how long each step took\n"
        "  --verbose   print the clang commands\n"
        "  --no-line   leave #line directives out of the C\n"
        "  --json      print errors as JSON lines on standard output, for editors\n"
        "  --stdin     read the file's text from standard input; the path names it in errors\n",
        stderr);
    return 2;
}

struct Options {
    std::string command;
    std::string input;
    std::string output;
    std::vector<std::string> program_args;
    std::vector<std::string> filters;  // `calx test`: the tests to run are those whose names hold one
    BuildMode mode = BuildMode::debug;
    bool library = false;
    bool timings = false;
    bool verbose = false;
    bool line_directives = true;
    bool json = false;
    bool stdin_text = false;
};

double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

bool is_command(const std::string& c) {
    return c == "check" || c == "build" || c == "run" || c == "test" || c == "eval" || c == "emit-c" || c == "expand";
}

// `calx [options] file.calx args...` is `calx run [options] file.calx -- args...`, as `python
// script.py args` is: options before the file are calx's, and everything after it is the
// program's, untouched, so a script's own `--release` or `-o` reaches it. It's what a `#!` line
// runs. The file is one that ends in `.calx`, or any that's there, for a script without the
// extension. 0 when it's set up, or the status to stop with.
int direct_run(int argc, char** argv, Options& opt) {
    int i = 1;
    for (; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--release") {
            opt.mode = BuildMode::release;
        } else if (a == "--timings") {
            opt.timings = true;
        } else if (a == "--verbose") {
            opt.verbose = true;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "calx: %s isn't an option for running a file; the options before one are --release, --timings and --verbose\n", a.c_str());
            return 2;
        } else {
            break;
        }
    }
    if (i == argc) return usage();
    std::string file = argv[i];
    std::error_code ec;
    bool there = std::filesystem::is_regular_file(file, ec);
    if (!there && std::filesystem::path(file).extension() != ".calx") {
        std::fprintf(stderr, "calx: `%s` isn't a command, or a file to run\n\n", file.c_str());
        usage();
        return 2;
    }
    opt.command = "run";
    opt.input = file;
    for (int j = i + 1; j < argc; j++) opt.program_args.push_back(argv[j]);
    return 0;
}

// CLX-87: runs each test the program was built with in a process of its own, so a panic or a
// crash fails only that test, as many at once as there are processors, and reports them in the
// order they're written, each as soon as those before it have. What a test wrote is shown only
// if it failed. 0 if every test passed.
int run_tests(const std::string& exe, const std::vector<std::string>& names, const std::vector<std::string>& filters) {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::size_t> chosen;
    for (std::size_t i = 0; i < names.size(); i++) {
        bool picked = filters.empty();
        for (const std::string& f : filters) picked = picked || names[i].find(f) != std::string::npos;
        if (picked) chosen.push_back(i);
    }
    if (chosen.empty()) {
        std::string list;
        for (const std::string& f : filters) list += (list.empty() ? "`" : ", `") + f + "`";
        std::fprintf(stderr, "calx: no test's name holds %s\n", list.c_str());
        return 1;
    }
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(cache_dir()) / "tests" / unique_stamp();
    std::filesystem::create_directories(dir, ec);
    struct Result {
        bool done = false;
        int status = 0;
        std::string output;
    };
    std::vector<Result> results(chosen.size());
    std::mutex mu;
    std::condition_variable ready;
    std::atomic<std::size_t> next{0};
    auto work = [&] {
        for (;;) {
            std::size_t k = next++;
            if (k >= chosen.size()) return;
            std::string out = (dir / (std::to_string(k) + ".txt")).string();
            int status = run_to_file({exe, "--calx-test=" + std::to_string(chosen[k])}, out);
            std::string text;
            read_file(out, text);
            std::error_code rm;
            std::filesystem::remove(out, rm);
            {
                std::lock_guard<std::mutex> lock(mu);
                results[k] = Result{true, status, std::move(text)};
            }
            ready.notify_all();
        }
    };
    unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    std::size_t workers = std::min<std::size_t>(cores, chosen.size());
    std::vector<std::thread> pool;
    for (std::size_t w = 0; w < workers; w++) pool.emplace_back(work);
    std::size_t passed = 0, failed = 0;
    for (std::size_t k = 0; k < chosen.size(); k++) {
        Result r;
        {
            std::unique_lock<std::mutex> lock(mu);
            ready.wait(lock, [&] { return results[k].done; });
            r = std::move(results[k]);
        }
        const std::string& name = names[chosen[k]];
        if (r.status == 0) {
            passed++;
            std::printf("ok    %s\n", name.c_str());
        } else {
            failed++;
            if (r.status < 0) {
                std::printf("FAIL  %s: it couldn't start\n", name.c_str());
            } else {
                std::printf("FAIL  %s: exit status %d\n", name.c_str(), r.status);
            }
            // What it wrote, indented, so each failure reads apart from the next.
            std::size_t at = 0;
            while (at < r.output.size()) {
                std::size_t end = r.output.find('\n', at);
                if (end == std::string::npos) end = r.output.size();
                std::string line = r.output.substr(at, end - at);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::printf("    %s\n", line.c_str());
                at = end + 1;
            }
        }
        std::fflush(stdout);
    }
    for (std::thread& t : pool) t.join();
    std::filesystem::remove_all(dir, ec);
    std::printf("%zu passed, %zu failed (%.2f s)\n", passed, failed, ms(t0) / 1000.0);
    return failed ? 1 : 0;
}

// A program read and checked, with the names, arena and types the checked program points into,
// which emitting it needs. A struct of its own, so `calx eval` can check two forms of its code
// and keep the one that compiles.
struct Front {
    Sources sources;
    Diagnostics diags{sources};
    Names names;
    Arena arena;
    Types types{arena};
    std::string home_error;
    std::string home = find_home(home_error);
    ParseContext pc{sources, names, arena, diags};
    std::unique_ptr<ModuleLoader> loader;
    Program program;
    bool expanded = true;
    std::vector<Expansion> shown;

    // Reads `file` and the modules it imports, runs their attributes, since what they generate is
    // checked with the rest (D124), and checks the program.
    void check(std::uint32_t file, const Options& opt, bool color) {
        loader = std::make_unique<ModuleLoader>(pc, sources, home, home_error);
        loader->load(file);
        if (!diags.has_errors()) {
            ExpandOptions xo;
            xo.home = home;
            xo.home_error = home_error;
            xo.verbose = opt.verbose;
            xo.json = opt.json;
            xo.color = color;
            if (opt.command == "expand") xo.shown = &shown;
            expanded = expand_attributes(pc, sources, *loader, xo);
        }
        std::vector<Module*> modules = loader->modules();
        if (!diags.has_errors() && expanded && opt.command != "expand") {
            check_program(CheckContext{sources, names, arena, diags, types}, modules, program,
                          opt.command != "check" && opt.command != "test", opt.library);
        }
    }

    bool ok() const { return !diags.has_errors() && expanded; }
};

// CLX-88: the code `calx eval` was given, taken apart: its imports, the standard modules it names
// without importing them, its statements before the last, and the last, which is printed if it
// has a value.
struct EvalCode {
    std::vector<std::string> imports;
    std::vector<std::string> body;
    std::string last;
    bool printable = false;  // the last statement may be an expression
};

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// The statements of `code`: split at line breaks and at `;`s outside strings, raw strings,
// brackets and comments, trimmed, without the empty ones.
std::vector<std::string> eval_statements(const std::string& code) {
    struct Ctx {
        char kind;      // 'c' code, 's' a string, 'r' a raw string
        int depth;      // in code, the brackets open
        int hashes;     // in a raw string, the `#`s that close it
    };
    std::vector<Ctx> stack = {{'c', 0, 0}};
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        std::size_t a = cur.find_first_not_of(" \t\r");
        std::size_t b = cur.find_last_not_of(" \t\r");
        if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
        cur.clear();
    };
    for (std::size_t i = 0; i < code.size(); i++) {
        char c = code[i];
        Ctx& top = stack.back();
        if (top.kind == 's') {
            cur += c;
            if (c == '\\' && i + 1 < code.size()) {
                cur += code[++i];
            } else if (c == '"') {
                stack.pop_back();
            } else if (c == '{') {
                stack.push_back({'c', 0, 0});
            }
            continue;
        }
        if (top.kind == 'r') {
            cur += c;
            if (c == '"' && code.compare(i + 1, top.hashes, std::string(top.hashes, '#')) == 0) {
                cur += code.substr(i + 1, top.hashes);
                i += top.hashes;
                stack.pop_back();
            }
            continue;
        }
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
            while (i < code.size() && code[i] != '\n') i++;
            i--;
            continue;
        }
        if (c == '#') {
            std::size_t j = i;
            while (j < code.size() && code[j] == '#') j++;
            if (j < code.size() && code[j] == '"') {
                cur += code.substr(i, j - i + 1);
                stack.push_back({'r', 0, static_cast<int>(j - i)});
                i = j;
                continue;
            }
        }
        if (c == '"') {
            cur += c;
            stack.push_back({'s', 0, 0});
            continue;
        }
        if (c == '(' || c == '[' || c == '{') top.depth++;
        if (c == ')' || c == ']' || c == '}') {
            if (top.depth == 0 && stack.size() > 1) {
                cur += c;
                stack.pop_back();  // the end of an interpolation
                continue;
            }
            top.depth--;
        }
        if ((c == ';' || c == '\n') && stack.size() == 1 && top.depth <= 0) {
            flush();
            continue;
        }
        cur += c;
    }
    flush();
    return out;
}

// Takes `code` apart for `calx eval`. A standard module it names, as `math.` in `math.sqrt(2.0)`,
// is imported for it, unless the code imports it or names a variable so.
EvalCode eval_code(const std::string& code, const std::string& home) {
    EvalCode e;
    std::vector<std::string> statements = eval_statements(code);
    for (std::string& s : statements) {
        if (s.rfind("import ", 0) == 0) {
            e.imports.push_back(s);
        } else {
            e.body.push_back(s);
        }
    }
    if (!e.body.empty()) {
        e.last = e.body.back();
        e.body.pop_back();
        static const char* const statements_only[] = {"let ", "var ", "for ", "while ", "loop", "return", "fn ", "struct ", "enum "};
        e.printable = true;
        for (const char* k : statements_only) {
            if (e.last.rfind(k, 0) == 0) e.printable = false;
        }
    }
    if (home.empty()) return e;
    // Each module's last name, as code names it, and the module: the one nearest `std` where two
    // share a name, as `std.math` and `std.java.math` do.
    std::map<std::string, std::pair<std::size_t, std::string>> modules;
    std::error_code ec;
    std::filesystem::path std_dir = std::filesystem::path(home) / "std";
    for (auto it = std::filesystem::recursive_directory_iterator(std_dir, ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->path().extension() != ".calx") continue;
        std::filesystem::path relative = std::filesystem::relative(it->path(), std_dir, ec);
        relative.replace_extension();
        std::string module = "std";
        std::size_t depth = 0;
        for (const auto& part : relative) {
            module += "." + part.string();
            depth++;
        }
        std::string name = it->path().stem().string();
        auto [at, fresh] = modules.emplace(name, std::make_pair(depth, module));
        if (!fresh && std::make_pair(depth, module) < at->second) at->second = std::make_pair(depth, module);
    }
    for (const auto& [name, found] : modules) {
        // Named as a module: `name.` with no letter, digit, `_` or `.` before it.
        bool named = false;
        for (std::size_t at = code.find(name + "."); at != std::string::npos; at = code.find(name + ".", at + 1)) {
            if (at == 0 || (!ident_char(code[at - 1]) && code[at - 1] != '.')) named = true;
        }
        if (!named) continue;
        bool taken = false;
        for (const std::string& s : e.imports) {
            taken = taken || (s.size() > name.size() && s.compare(s.size() - name.size() - 1, std::string::npos, "." + name) == 0);
        }
        for (const char* k : {"let ", "var "}) {
            for (const char* after : {" ", "=", ":"}) taken = taken || code.find(k + name + after) != std::string::npos;
        }
        if (!taken) e.imports.push_back("import " + found.second);
    }
    return e;
}

// The program `calx eval` builds: the imports, then a `main` of the statements, with the last
// printed, if `print`, or as it is.
std::string eval_program(const EvalCode& e, bool print) {
    std::string out;
    for (const std::string& s : e.imports) out += s + "\n";
    out += "fn main() {\n";
    for (const std::string& s : e.body) out += "    " + s + "\n";
    if (!e.last.empty()) {
        if (print && e.printable) {
            out += "    let value = " + e.last + "\n    println(\"{value}\")\n";
        } else {
            out += "    " + e.last + "\n";
        }
    }
    out += "}\n";
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    Options opt;
    opt.command = argv[1];
    if (opt.command == "version" || opt.command == "--version") {
        print_version();
        return 0;
    }
    if (opt.command == "help" || opt.command == "--help" || opt.command == "-h") {
        usage();
        return 0;
    }
    if (opt.command == "update") return run_update(std::vector<std::string>(argv + 2, argv + argc));
    if (opt.command == "doctor") return run_doctor();
    std::vector<std::string> rest(argv + 2, argv + argc);
    if (opt.command == "clean") return run_clean(rest);
    if (opt.command == "uninstall") return run_uninstall(rest);
    if (opt.command == "completions") return run_completions(rest);
    if (opt.command == "new") return run_new(rest);
    if (opt.command == "fmt") return run_fmt(rest);
    bool direct = !is_command(opt.command);
    if (direct) {
        if (int status = direct_run(argc, argv, opt)) return status;
    }
    for (int i = 2; i < argc && !direct; i++) {
        std::string a = argv[i];
        if (a == "--") {
            for (int j = i + 1; j < argc; j++) opt.program_args.push_back(argv[j]);
            break;
        } else if (a == "--release") {
            opt.mode = BuildMode::release;
        } else if (a == "--lib") {
            opt.library = true;
        } else if (a == "--timings") {
            opt.timings = true;
        } else if (a == "--verbose") {
            opt.verbose = true;
        } else if (a == "--no-line") {
            opt.line_directives = false;
        } else if (a == "--json") {
            opt.json = true;
        } else if (a == "--stdin") {
            opt.stdin_text = true;
        } else if (a == "-o" && i + 1 < argc) {
            opt.output = argv[++i];
        } else if (!a.empty() && a[0] == '-' && !(opt.command == "eval" && opt.input.empty())) {
            std::fprintf(stderr, "calx: unknown option %s\n", a.c_str());
            return 2;
        } else if (opt.input.empty()) {
            opt.input = a;
        } else if (opt.command == "test") {
            opt.filters.push_back(a);
        } else if (opt.command == "eval") {
            std::fprintf(stderr, "calx: `calx eval` takes the code as one argument; put it in quotes, and give the program's "
                                 "arguments after --\n");
            return 2;
        } else {
            std::fprintf(stderr, "calx: only one input file is supported for now\n");
            return 2;
        }
    }
    if (opt.input.empty()) return usage();
    if (!opt.json) update_notice();
    if (opt.library && opt.command == "run") {
        std::fprintf(stderr, "calx: a library doesn't run by itself; build it with `calx build --lib`, and open it from a program\n");
        return 2;
    }

    auto t0 = std::chrono::steady_clock::now();
    bool color = isatty(fileno(stderr)) != 0;
    auto front = std::make_unique<Front>();
    if (opt.command == "eval") {
        // The code's last statement is printed if it has a value. If the program that prints it
        // doesn't compile, the code as it is may: its last statement is a call that returns
        // nothing, or an assignment.
        EvalCode code = eval_code(opt.input, front->home);
        front->check(front->sources.add("eval", eval_program(code, true)), opt, color);
        if (!front->ok() && code.printable) {
            auto plain = std::make_unique<Front>();
            plain->check(plain->sources.add("eval", eval_program(code, false)), opt, color);
            if (plain->ok()) front = std::move(plain);
        }
    } else {
        std::uint32_t file;
        if (opt.stdin_text) {
            // An editor's unsaved text, checked as if it were the file.
#ifdef _WIN32
            _setmode(_fileno(stdin), _O_BINARY);
#endif
            std::string text((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
            file = front->sources.add(opt.input, std::move(text));
        } else {
            file = front->sources.load(opt.input);
        }
        if (file == 0) {
            std::fprintf(stderr, "calx: can't read %s\n", opt.input.c_str());
            return 1;
        }
        front->check(file, opt, color);
    }
    Sources& sources = front->sources;
    Diagnostics& diags = front->diags;
    Names& names = front->names;
    Program& program = front->program;
    bool expanded = front->expanded;
    std::vector<Expansion>& shown = front->shown;
    if (opt.json) {
        diags.print_json(stdout);
        std::fflush(stdout);
        if (diags.has_errors() || !expanded) return 1;
    }
    if (diags.has_errors()) {
        diags.print(stderr, color);
        std::size_t n = diags.error_count();
        std::fprintf(stderr, "%zu error%s\n", n, n == 1 ? "" : "s");
        return 1;
    }
    if (!expanded) return 1;
    if (!opt.json) diags.print(stderr, color);
    if (opt.command == "expand") {
        if (shown.empty()) std::printf("// %s uses no attributes that generate code\n", opt.input.c_str());
        for (std::size_t i = 0; i < shown.size(); i++) {
            std::printf("%s// %s, in %s\n%s", i ? "\n" : "", shown[i].what.c_str(), shown[i].path.c_str(),
                        shown[i].code.c_str());
        }
        return 0;
    }
    double front_ms = ms(t0);
    std::vector<std::string> tests;
    for (const Item* t : program.tests) tests.emplace_back(names.text(t->name));
    if (opt.command == "test" && tests.empty()) {
        std::fprintf(stderr, "calx: %s has no tests; a test is a function marked `@test`\n", opt.input.c_str());
        return 1;
    }
    if (opt.command == "check") {
        if (opt.timings) std::fprintf(stderr, "check: %.1f ms\n", front_ms);
        return 0;
    }

    auto t1 = std::chrono::steady_clock::now();
    EmitOptions eo;
    eo.line_directives = opt.line_directives;
    eo.library = opt.library;
    eo.tests = opt.command == "test";
    std::vector<CFile> files = emit_c(program, sources, names, eo);
    double emit_ms = ms(t1);
    if (opt.command == "emit-c") {
        // One module's C goes to the output as it is. With several, each starts with a line
        // naming its file, or `-o` names a folder to write them in.
        if (opt.output.empty()) {
            for (const CFile& f : files) {
                if (files.size() > 1) std::printf("// ---- %s\n", f.name.c_str());
                std::fwrite(f.text.data(), 1, f.text.size(), stdout);
            }
        } else if (files.size() == 1) {
            std::error_code ec;
            std::filesystem::path parent = std::filesystem::path(opt.output).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent, ec);
            if (!write_file_atomic(opt.output, files[0].text)) {
                std::fprintf(stderr, "calx: can't write %s\n", opt.output.c_str());
                return 1;
            }
        } else {
            std::error_code ec;
            std::filesystem::create_directories(opt.output, ec);
            for (const CFile& f : files) {
                std::string path = (std::filesystem::path(opt.output) / f.name).string();
                if (!write_file_atomic(path, f.text)) {
                    std::fprintf(stderr, "calx: can't write %s\n", path.c_str());
                    return 1;
                }
            }
        }
        return 0;
    }

    Toolchain tc;
    std::string error;
    if (!find_toolchain(tc, error)) {
        std::fprintf(stderr, "calx: %s\n", error.c_str());
        return 1;
    }
    std::vector<std::string> c_sources;
    for (CFile& f : files) c_sources.push_back(std::move(f.text));
    BuildResult b = build_program(tc, c_sources, opt.mode, opt.library ? BuildKind::library : BuildKind::program, opt.verbose);
    if (!b.ok) return 1;
    if (opt.timings) {
        std::fprintf(stderr, "front end: %.1f ms, emit: %.1f ms, clang: %.1f ms, link: %.1f ms%s\n", front_ms, emit_ms,
                     b.compile_ms, b.link_ms, b.reused ? " (reused the cached program)" : "");
    }
    if (opt.command == "build") {
        std::string out = opt.output;
        if (out.empty()) {
            out = std::filesystem::path(opt.input).stem().string();
            if (opt.library) {
                out += library_suffix();
            } else {
#ifdef _WIN32
                out += ".exe";
#endif
            }
        }
        std::string why;
        if (!place_build(b.exe, out, why)) {
            std::fprintf(stderr, "calx: %s\n", why.c_str());
            return 1;
        }
        return 0;
    }
    if (opt.command == "test") {
        set_run_environment(opt.input);
        return run_tests(b.exe, tests, opt.filters);
    }
    if (opt.command == "eval") {
        std::vector<std::string> run = {b.exe};
        run.insert(run.end(), opt.program_args.begin(), opt.program_args.end());
        std::fflush(stdout);
        std::fflush(stderr);
        int status = run_process(run, "eval");
        if (status < 0) {
            std::fprintf(stderr, "calx: can't run %s\n", b.exe.c_str());
            return 1;
        }
        return status;
    }
    std::vector<std::string> run = {b.exe};
    run.insert(run.end(), opt.program_args.begin(), opt.program_args.end());
    std::fflush(stdout);
    std::fflush(stderr);
    // The program sees the file it was built from as its `argv[0]`, as a Python script sees
    // `sys.argv[0]`, rather than the cached program's path, so `std.env.program()` names it.
    // Its full path is `std.env.script()`, as Python's `__file__` is, and this compiler's is in
    // CALX, as Python's `sys.executable` is.
    set_run_environment(opt.input);
    int status = run_process(run, opt.input);
    if (status < 0) {
        std::fprintf(stderr, "calx: can't run %s\n", b.exe.c_str());
        return 1;
    }
    return status;
}
