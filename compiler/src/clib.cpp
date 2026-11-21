// Static libraries for hosts that aren't Calx (CLX-334): putting a build's objects in a `.lib` or
// a `.a`, with whichever archiver the system has.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "target.h"
#include "toolchain.h"

namespace calx {

namespace fs = std::filesystem;

const char* staticlib_suffix(Target t) { return t.os == Os::windows ? ".lib" : ".a"; }

std::string staticlib_name(Target t, const std::string& stem) {
    return t.os == Os::windows ? stem + ".lib" : "lib" + stem + ".a";
}

namespace {

// `name`, with `.exe` on Windows, beside clang, or else on PATH, or "".
std::string tool(const Toolchain& tc, const std::string& name) {
    std::string file = name + exe_suffix(kHost.os);
    fs::path beside = fs::path(tc.clang).parent_path() / file;
    std::error_code ec;
    if (fs::is_regular_file(beside, ec)) return beside.string();
    return which(name);
}

// Quotes `a` as a Windows command line does, for a response file.
std::string quoted(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string s = "\"";
    std::size_t slashes = 0;
    for (char c : a) {
        if (c == '\\') {
            slashes++;
            continue;
        }
        s.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
        s += c;
        slashes = 0;
    }
    s.append(slashes * 2, '\\');
    return s + "\"";
}

}  // namespace

bool archive_objects(const Toolchain& tc, const std::vector<std::string>& objects, const std::string& output, bool verbose) {
    std::vector<std::string> head;  // the archiver and its options, before the objects
    bool windows_style = false;     // it takes `/out:`, and a response file of the objects
    std::error_code ec;
    if (kHost.os == Os::windows) {
        // llvm-lib, or lld as lld-link, which is llvm-lib when its first argument is `/lib`, as
        // the toolchain a release carries has it, or Visual Studio's own lib.exe. Each writes an
        // archive in Visual Studio's format, with the symbol table its linker reads.
        if (std::string lib = tool(tc, "llvm-lib"); !lib.empty()) {
            head = {lib, "/nologo"};
        } else if (!tc.lld_path.empty()) {
            head = {tc.lld_path, "-flavor", "link", "/lib", "/nologo"};
        } else if (std::string link = tool(tc, "lld-link"); !link.empty()) {
            head = {link, "/lib", "/nologo"};
        } else if (std::string ms = which("lib"); !ms.empty()) {
            head = {ms, "/nologo"};
        }
        windows_style = !head.empty();
    } else if (kHost.os == Os::macos) {
        // libtool, Apple's way to make one, with the table of contents ld64 wants; or llvm-ar.
        if (std::string lt = which("libtool"); !lt.empty() && fs::path(lt).parent_path() == "/usr/bin") {
            head = {lt, "-static", "-no_warning_for_no_symbols", "-o", output};
        } else if (std::string ar = tool(tc, "llvm-ar"); !ar.empty()) {
            head = {ar, "rcs", "--format=darwin", output};
        }
    } else {
        for (const char* name : {"llvm-ar", "ar"}) {
            if (std::string ar = tool(tc, name); !ar.empty() && head.empty()) head = {ar, "rcs", output};
        }
    }
    if (head.empty()) {
        std::fprintf(stderr, "calx: can't find a program to make a static library with: %s\n",
                     kHost.os == Os::windows ? "it takes llvm-lib or lld-link beside clang or on PATH, or Visual Studio's lib.exe"
                     : kHost.os == Os::macos ? "it takes libtool, from Apple's Command Line Tools (`xcode-select --install`)"
                                               : "it takes ar, from binutils, or llvm-ar beside clang");
        return false;
    }
    // An archive is added to, not replaced, so it's made fresh.
    fs::remove(long_path(output), ec);
    std::vector<std::string> cmd = head;
    std::string rsp = output + ".rsp";
    if (windows_style) {
        // A response file of every object, since a library of a few hundred modules from a cache at
        // a long path passes the 32,767 characters a Windows command line can be (CLX-208).
        cmd.push_back("/out:" + output);
        std::string text;
        for (const std::string& o : objects) text += quoted(o) + "\n";
        if (!write_file_atomic(rsp, text)) {
            std::fprintf(stderr, "calx: can't write %s\n", rsp.c_str());
            return false;
        }
        cmd.push_back("@" + rsp);
    } else {
        cmd.insert(cmd.end(), objects.begin(), objects.end());
    }
    if (verbose) {
        for (const std::string& a : cmd) std::fprintf(stderr, "%s ", a.c_str());
        std::fputc('\n', stderr);
    }
    std::string log = output + ".log";
    int status = run_to_file(cmd, log);
    std::string said;
    read_file(log, said);
    fs::remove(log, ec);
    if (windows_style) fs::remove(rsp, ec);
    if (status != 0) {
        std::fprintf(stderr, "%s", said.c_str());
        std::fprintf(stderr, "calx: %s couldn't make the static library (status %d)\n", fs::path(cmd[0]).filename().string().c_str(),
                     status);
        return false;
    }
    if (verbose) std::fprintf(stderr, "%s", said.c_str());
    return true;
}

}  // namespace calx
