// The `calx` command.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "check.h"
#include "emit.h"
#include "parser.h"
#include "source.h"
#include "toolchain.h"

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

using namespace calx;

namespace {

constexpr const char* kVersion = "0.1.0";

int usage() {
    std::fputs(
        "usage: calx <command> [options] <file.calx>\n"
        "\n"
        "commands:\n"
        "  check     check a program without building it\n"
        "  build     build a program (-o names the output)\n"
        "  run       build a program and run it; arguments after -- go to the program\n"
        "  emit-c    print the C a program compiles to (-o names a file)\n"
        "  version   print the compiler's version\n"
        "\n"
        "options:\n"
        "  --release   optimize (the default is a debug build, which builds fastest)\n"
        "  -o PATH     where build and emit-c write\n"
        "  --timings   print how long each step took\n"
        "  --verbose   print the clang commands\n"
        "  --no-line   leave #line directives out of the C\n",
        stderr);
    return 2;
}

struct Options {
    std::string command;
    std::string input;
    std::string output;
    std::vector<std::string> program_args;
    BuildMode mode = BuildMode::debug;
    bool timings = false;
    bool verbose = false;
    bool line_directives = true;
};

double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    Options opt;
    opt.command = argv[1];
    if (opt.command == "version" || opt.command == "--version") {
        std::printf("calx %s\n", kVersion);
        return 0;
    }
    if (opt.command == "help" || opt.command == "--help" || opt.command == "-h") {
        usage();
        return 0;
    }
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--") {
            for (int j = i + 1; j < argc; j++) opt.program_args.push_back(argv[j]);
            break;
        } else if (a == "--release") {
            opt.mode = BuildMode::release;
        } else if (a == "--timings") {
            opt.timings = true;
        } else if (a == "--verbose") {
            opt.verbose = true;
        } else if (a == "--no-line") {
            opt.line_directives = false;
        } else if (a == "-o" && i + 1 < argc) {
            opt.output = argv[++i];
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "calx: unknown option %s\n", a.c_str());
            return 2;
        } else if (opt.input.empty()) {
            opt.input = a;
        } else {
            std::fprintf(stderr, "calx: only one input file is supported for now\n");
            return 2;
        }
    }
    bool known = opt.command == "check" || opt.command == "build" || opt.command == "run" || opt.command == "emit-c";
    if (!known || opt.input.empty()) return usage();

    auto t0 = std::chrono::steady_clock::now();
    Sources sources;
    Diagnostics diags(sources);
    std::uint32_t file = sources.load(opt.input);
    if (file == 0) {
        std::fprintf(stderr, "calx: can't read %s\n", opt.input.c_str());
        return 1;
    }
    Names names;
    Arena arena;
    Types types(arena);
    Module* module = parse_file(ParseContext{sources, names, arena, diags}, file);
    Program program;
    if (!diags.has_errors()) {
        check_module(CheckContext{sources, names, arena, diags, types}, module, program, opt.command != "check");
    }
    bool color = isatty(fileno(stderr)) != 0;
    if (diags.has_errors()) {
        diags.print(stderr, color);
        std::size_t n = diags.error_count();
        std::fprintf(stderr, "%zu error%s\n", n, n == 1 ? "" : "s");
        return 1;
    }
    diags.print(stderr, color);
    double front_ms = ms(t0);
    if (opt.command == "check") {
        if (opt.timings) std::fprintf(stderr, "check: %.1f ms\n", front_ms);
        return 0;
    }

    auto t1 = std::chrono::steady_clock::now();
    EmitOptions eo;
    eo.line_directives = opt.line_directives;
    std::string c = emit_c(program, sources, names, eo);
    double emit_ms = ms(t1);
    if (opt.command == "emit-c") {
        if (opt.output.empty()) {
            std::fwrite(c.data(), 1, c.size(), stdout);
        } else if (!write_file_atomic(opt.output, c)) {
            std::fprintf(stderr, "calx: can't write %s\n", opt.output.c_str());
            return 1;
        }
        return 0;
    }

    Toolchain tc;
    std::string error;
    if (!find_toolchain(tc, error)) {
        std::fprintf(stderr, "calx: %s\n", error.c_str());
        return 1;
    }
    BuildResult b = build_program(tc, c, opt.mode, opt.verbose);
    if (!b.ok) return 1;
    if (opt.timings) {
        std::fprintf(stderr, "front end: %.1f ms, emit: %.1f ms, clang: %.1f ms, link: %.1f ms%s\n", front_ms, emit_ms,
                     b.compile_ms, b.link_ms, b.reused ? " (reused the cached program)" : "");
    }
    if (opt.command == "build") {
        std::string out = opt.output;
        if (out.empty()) {
            out = std::filesystem::path(opt.input).stem().string();
#ifdef _WIN32
            out += ".exe";
#endif
        }
        std::error_code ec;
        std::filesystem::copy_file(b.exe, out, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::fprintf(stderr, "calx: can't write %s: %s\n", out.c_str(), ec.message().c_str());
            return 1;
        }
        return 0;
    }
    std::vector<std::string> run = {b.exe};
    run.insert(run.end(), opt.program_args.begin(), opt.program_args.end());
    std::fflush(stdout);
    std::fflush(stderr);
    int status = run_process(run);
    if (status < 0) {
        std::fprintf(stderr, "calx: can't run %s\n", b.exe.c_str());
        return 1;
    }
    return status;
}
