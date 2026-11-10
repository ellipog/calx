#include "manage.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "term.h"
#include "toolchain.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace calx {

namespace {

// A size in bytes, as people read one: `412.3 MB`.
std::string human(std::uintmax_t bytes) {
    char buf[32];
    if (bytes < 1000 * 1000) {
        std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(bytes) / 1000.0);
    } else if (bytes < 1000ull * 1000 * 1000) {
        std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / 1e6);
    } else {
        std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(bytes) / 1e9);
    }
    return buf;
}

// Removes everything in `dir`, and `dir` itself if it can, counting the bytes of the files it
// removed and the files it couldn't, which on Windows are those a running program holds open.
void remove_counting(const fs::path& dir, std::uintmax_t& freed, std::size_t& kept) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e;
        if (it->is_directory(e) && !it->is_symlink(e)) {
            dirs.push_back(it->path());
            continue;
        }
        std::uintmax_t size = it->is_regular_file(e) ? it->file_size(e) : 0;
        if (fs::remove(it->path(), e)) {
            freed += size;
        } else {
            kept++;
        }
    }
    // The deepest first, so each is empty when it's removed; one that still holds a kept file stays.
    for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) {
        std::error_code e;
        fs::remove(*d, e);
    }
    std::error_code e;
    fs::remove(dir, e);
}

// Whether standard input is a terminal someone can answer in. On Windows `_isatty` says so for the
// NUL device too, so it asks for the console's mode instead.
bool stdin_is_terminal() {
#ifdef _WIN32
    DWORD mode;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

bool has_flag(const std::vector<std::string>& args, const char* flag) {
    for (const std::string& a : args) {
        if (a == flag) return true;
    }
    return false;
}

#ifdef _WIN32
std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

// A function of a system DLL that only `calx uninstall` needs, loaded when it does: a program
// that imports USER32 takes about 3 ms longer to start, and one that imports ADVAPI32 about half a
// millisecond, which every `calx` would.
template <typename F>
F system_fn(const wchar_t* dll, const char* name) {
    HMODULE m = LoadLibraryW(dll);
    return m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

// Takes `bin` out of the user's Path in the registry, where the install script put it, and tells
// running programs the environment changed. Whether it was there.
bool remove_from_path(const fs::path& bin) {
    auto RegOpenKeyExW = system_fn<decltype(&::RegOpenKeyExW)>(L"advapi32.dll", "RegOpenKeyExW");
    auto RegQueryValueExW = system_fn<decltype(&::RegQueryValueExW)>(L"advapi32.dll", "RegQueryValueExW");
    auto RegSetValueExW = system_fn<decltype(&::RegSetValueExW)>(L"advapi32.dll", "RegSetValueExW");
    auto RegCloseKey = system_fn<decltype(&::RegCloseKey)>(L"advapi32.dll", "RegCloseKey");
    if (!RegOpenKeyExW || !RegQueryValueExW || !RegSetValueExW || !RegCloseKey) return false;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    bool removed = false;
    if (RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size > 0) {
        std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, L"Path", nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size) == ERROR_SUCCESS) {
            value.resize(wcsnlen(value.c_str(), value.size()));
            std::wstring target = lower(bin.wstring());
            std::wstring kept;
            std::size_t at = 0;
            while (at <= value.size()) {
                std::size_t end = value.find(L';', at);
                if (end == std::wstring::npos) end = value.size();
                std::wstring part = value.substr(at, end - at);
                if (!part.empty()) {
                    if (lower(part) == target) {
                        removed = true;
                    } else {
                        if (!kept.empty()) kept += L';';
                        kept += part;
                    }
                }
                at = end + 1;
            }
            if (removed) {
                RegSetValueExW(key, L"Path", 0, type == REG_EXPAND_SZ ? REG_EXPAND_SZ : REG_SZ,
                               reinterpret_cast<const BYTE*>(kept.c_str()), static_cast<DWORD>((kept.size() + 1) * sizeof(wchar_t)));
                if (auto send = system_fn<decltype(&::SendMessageTimeoutW)>(L"user32.dll", "SendMessageTimeoutW")) {
                    DWORD_PTR result;
                    send(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 2000,
                         &result);
                }
            }
        }
    }
    RegCloseKey(key);
    return removed;
}

// Removes `home` once this program has ended, since Windows won't remove a running calx.exe or
// the folder it's in: a batch file in the temporary folder removes the folder, then itself. It
// tries each second for 30 seconds, since this calx can take a moment to end and a virus scanner
// can hold a file it has just seen (CLX-201). Whether it started.
bool remove_after_exit(const fs::path& home) {
    wchar_t temp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, temp);
    if (n == 0 || n > MAX_PATH) return false;
    fs::path script = fs::path(temp) / ("calx-uninstall-" + unique_stamp() + ".cmd");
    // UTF-8, which `chcp 65001` makes cmd read the lines after it in, for a home with any name.
    std::wstring w = home.wstring();
    int len8 = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string home8(static_cast<std::size_t>(len8), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), home8.data(), len8, nullptr, nullptr);
    // cmd expands `%` in a batch file's lines even in quotes, so a folder such as `100%` needs `%%`.
    std::string quoted;
    for (char c : home8) quoted += c == '%' ? std::string("%%") : std::string(1, c);
    // `ping -n 2` waits a second. The test for the folder is a line of its own, outside any
    // parentheses, so `%tries%` is read each time round.
    std::string text = "@echo off\r\nchcp 65001 > nul\r\nset tries=0\r\n:again\r\nping -n 2 127.0.0.1 > nul\r\nrmdir /s /q \"" +
                       quoted + "\" 2> nul\r\nif not exist \"" + quoted + "\\\" goto done\r\nset /a tries+=1\r\nif %tries% lss 30 goto again\r\n" +
                       ":done\r\ndel \"%~f0\"\r\n";
    if (!write_file_atomic(script.string(), text)) return false;
    wchar_t system[MAX_PATH + 1];
    UINT len = GetSystemDirectoryW(system, MAX_PATH + 1);
    if (len == 0 || len > MAX_PATH) return false;
    std::string cmd = (fs::path(system) / "cmd.exe").string();
    return start_detached({cmd, "/d", "/c", script.string()});
}
#else
// Takes the lines the install script added out of each shell startup file: `export PATH="BIN:$PATH"`,
// with the `# Calx` line above it and the blank line above that. The files it changed.
std::vector<std::string> remove_from_path(const fs::path& bin) {
    std::vector<std::string> changed;
    const char* home = std::getenv("HOME");
    if (!home || !*home) return changed;
    std::string line = "export PATH=\"" + bin.string() + ":$PATH\"";
    for (const char* name : {".profile", ".bashrc", ".zshrc"}) {
        fs::path rc = fs::path(home) / name;
        std::string text;
        if (!read_file(rc.string(), text)) continue;
        std::vector<std::string> lines;
        std::size_t at = 0;
        while (at < text.size()) {
            std::size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            lines.push_back(text.substr(at, end - at));
            at = end + 1;
        }
        bool removed = false;
        std::vector<std::string> out;
        for (const std::string& l : lines) {
            if (l != line) {
                out.push_back(l);
                continue;
            }
            removed = true;
            if (!out.empty() && out.back() == "# Calx") out.pop_back();
            if (!out.empty() && out.back().empty()) out.pop_back();
        }
        if (!removed) continue;
        std::string joined;
        for (const std::string& l : out) joined += l + "\n";
        if (write_file_atomic(rc.string(), joined)) changed.push_back(rc.string());
    }
    return changed;
}
#endif

}  // namespace

int run_clean(const std::vector<std::string>& args) {
    if (!args.empty()) {
        std::fprintf(stderr, "calx clean: takes no arguments\n");
        return 2;
    }
    fs::path dir = cache_dir();
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        std::printf("The build cache is empty: nothing is in %s.\n", dir.string().c_str());
        return 0;
    }
    std::uintmax_t freed = 0;
    std::size_t kept = 0;
    remove_counting(dir, freed, kept);
    std::printf("Removed %s of builds from %s.\n", human(freed).c_str(), dir.string().c_str());
    if (kept) {
        std::printf("%zu file%s held by programs that are running %s kept; `calx clean` again once they've ended removes %s.\n",
                    kept, kept == 1 ? "" : "s", kept == 1 ? "was" : "were", kept == 1 ? "it" : "them");
    }
    return 0;
}

int run_uninstall(const std::vector<std::string>& args) {
    for (const std::string& a : args) {
        if (a != "--yes") {
            std::fprintf(stderr, "calx uninstall: unknown option %s; `--yes` removes Calx without asking\n", a.c_str());
            return 2;
        }
    }
    fs::path exe = self_exe();
    fs::path home = exe.parent_path().parent_path();
    std::error_code ec;
    // An installation is a bin/ with calx in it, beside runtime/ and std/, and no compiler/, which
    // a source tree has.
    bool installed = exe.parent_path().filename() == "bin" && fs::is_directory(home / "runtime", ec) &&
                     fs::is_directory(home / "std", ec) && !fs::exists(home / "compiler", ec);
    if (!installed) {
        std::fprintf(stderr,
                     "calx uninstall: this calx (%s) wasn't installed by the install script, so there's nothing for it to "
                     "remove; delete the folder it's in, if that's what you want.\n",
                     exe.string().c_str());
        return 1;
    }
    fs::path bin = exe.parent_path();
    fs::path cache = cache_dir();
    bool path = std::getenv("CALX_NO_MODIFY_PATH") == nullptr || std::string(std::getenv("CALX_NO_MODIFY_PATH")) != "1";
    std::printf("This removes Calx from %s, its build cache in %s%s.\n", home.string().c_str(), cache.string().c_str(),
                path ? (", and " + bin.string() + " from your PATH").c_str() : "");
    if (!has_flag(args, "--yes")) {
        if (!stdin_is_terminal()) {
            std::fprintf(stderr, "calx uninstall: give `--yes` to remove it without asking\n");
            return 1;
        }
        std::printf("Remove it? [y/N] ");
        std::fflush(stdout);
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y" && answer != "yes") {
            std::printf("Nothing was removed.\n");
            return 1;
        }
    }
    if (path) {
#ifdef _WIN32
        if (remove_from_path(bin)) std::printf("Took %s out of your PATH.\n", bin.string().c_str());
#else
        for (const std::string& rc : remove_from_path(bin)) std::printf("Took %s out of your PATH in %s.\n", bin.string().c_str(), rc.c_str());
#endif
    }
    std::uintmax_t freed = 0;
    std::size_t kept = 0;
    if (fs::exists(cache, ec)) remove_counting(cache, freed, kept);
#ifdef _WIN32
    if (!remove_after_exit(home)) {
        std::fprintf(stderr, "calx uninstall: couldn't start removing %s; delete it once this ends\n", home.string().c_str());
        return 1;
    }
    std::printf("Calx is removed; its folder goes in a moment, once this command has ended. Open a new terminal for PATH to change.\n");
#else
    fs::remove_all(home, ec);
    if (ec) {
        std::fprintf(stderr, "calx uninstall: couldn't remove %s: %s\n", home.string().c_str(), ec.message().c_str());
        return 1;
    }
    std::printf("Calx is removed. Open a new terminal for PATH to change.\n");
#endif
    return 0;
}

namespace {

struct Command {
    const char* name;
    const char* about;
};

// The commands, as `calx help` lists them, for the completions. No description has a `'`, which
// would end the quotes each shell's script puts it in.
const Command kCommands[] = {
    {"new", "start a program"},
    {"check", "check a program without building it"},
    {"build", "build a program"},
    {"run", "build a program and run it"},
    {"test", "run the @test functions of a file"},
    {"eval", "run code given as one argument"},
    {"emit-c", "print the C a program compiles to"},
    {"expand", "print the code attributes generate"},
    {"fmt", "format files in the one Calx format"},
    {"serve", "serve a folder over HTTP"},
    {"update", "install the newest release"},
    {"doctor", "check that Calx can build and run programs"},
    {"clean", "remove the build cache"},
    {"uninstall", "remove Calx"},
    {"completions", "print completions for a shell"},
    {"version", "print the version of calx"},
    {"help", "list the commands"},
};

const char* const kBuildOptions = "--release --lib -o --timings --verbose --no-line --json --stdin";
const char* const kUpdateOptions = "--check --nightly --stable --to --rollback --force";
const char* const kServeOptions = "--port --bind";
const char* const kShells = "bash zsh fish powershell";

std::string command_names() {
    std::string out;
    for (const Command& c : kCommands) out += std::string(out.empty() ? "" : " ") + c.name;
    return out;
}

std::string bash() {
    return "# calx completions for bash: add `eval \"$(calx completions bash)\"` to ~/.bashrc\n"
           "_calx() {\n"
           "    local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
           "    if [ \"$COMP_CWORD\" -eq 1 ]; then\n"
           "        COMPREPLY=($(compgen -W \"" + command_names() + "\" -- \"$cur\") $(compgen -f -X '!*.calx' -- \"$cur\") $(compgen -d -- \"$cur\"))\n"
           "        return\n"
           "    fi\n"
           "    case \"${COMP_WORDS[1]}\" in\n"
           "        update) COMPREPLY=($(compgen -W \"" + std::string(kUpdateOptions) + "\" -- \"$cur\")) ;;\n"
           "        completions) COMPREPLY=($(compgen -W \"" + std::string(kShells) + "\" -- \"$cur\")) ;;\n"
           "        uninstall) COMPREPLY=($(compgen -W \"--yes\" -- \"$cur\")) ;;\n"
           "        serve)\n"
           "            if [[ \"$cur\" == -* ]]; then\n"
           "                COMPREPLY=($(compgen -W \"" + std::string(kServeOptions) + "\" -- \"$cur\"))\n"
           "            else\n"
           "                COMPREPLY=($(compgen -d -- \"$cur\"))\n"
           "            fi ;;\n"
           "        clean|doctor|version|help) COMPREPLY=() ;;\n"
           "        *)\n"
           "            if [[ \"$cur\" == -* ]]; then\n"
           "                COMPREPLY=($(compgen -W \"" + std::string(kBuildOptions) + "\" -- \"$cur\"))\n"
           "            else\n"
           "                COMPREPLY=($(compgen -f -X '!*.calx' -- \"$cur\") $(compgen -d -- \"$cur\"))\n"
           "            fi ;;\n"
           "    esac\n"
           "}\n"
           "complete -o filenames -F _calx calx\n";
}

std::string zsh() {
    std::string commands;
    for (const Command& c : kCommands) commands += std::string("        '") + c.name + ":" + c.about + "'\n";
    std::string options;
    for (const char* o : {"--release[optimize]", "--lib[build a library]", "-o[where to write]:file:_files", "--timings[print how long each step took]",
                          "--verbose[print the clang commands]", "--no-line[leave #line directives out]", "--json[print errors as JSON]",
                          "--stdin[read the text from standard input]"}) {
        options += std::string(" '") + o + "'";
    }
    return "#compdef calx\n"
           "# calx completions for zsh: add `eval \"$(calx completions zsh)\"` to ~/.zshrc, after compinit\n"
           "_calx() {\n"
           "    local -a commands\n"
           "    commands=(\n" + commands + "    )\n"
           "    if (( CURRENT == 2 )); then\n"
           "        _describe 'command' commands\n"
           "        _files -g '*.calx'\n"
           "        return\n"
           "    fi\n"
           "    case $words[2] in\n"
           "        update) _values 'option' " + std::string(kUpdateOptions) + " ;;\n"
           "        completions) _values 'shell' " + std::string(kShells) + " ;;\n"
           "        uninstall) _values 'option' --yes ;;\n"
           "        serve) _arguments '--port[the port]:port:' '--bind[the address to listen on]:address:' '*:folder:_files -/' ;;\n"
           "        clean|doctor|version|help) ;;\n"
           "        *) _arguments" + options + " '*:file:_files -g \"*.calx\"' ;;\n"
           "    esac\n"
           "}\n"
           "compdef _calx calx\n";
}

std::string fish() {
    std::string out = "# calx completions for fish: `calx completions fish > ~/.config/fish/completions/calx.fish`\n"
                      "complete -c calx -f\n";
    for (const Command& c : kCommands) {
        out += std::string("complete -c calx -n __fish_use_subcommand -a ") + c.name + " -d '" + c.about + "'\n";
    }
    const char* builds = "check build run test emit-c expand";
    out += std::string("complete -c calx -n '__fish_use_subcommand' -a '(__fish_complete_suffix .calx)'\n");
    out += std::string("complete -c calx -n '__fish_seen_subcommand_from ") + builds + "' -a '(__fish_complete_suffix .calx)'\n";
    for (const char* o : {"release", "lib", "timings", "verbose", "no-line", "json", "stdin"}) {
        out += std::string("complete -c calx -n '__fish_seen_subcommand_from ") + builds + "' -l " + o + "\n";
    }
    out += std::string("complete -c calx -n '__fish_seen_subcommand_from ") + builds + "' -s o -r -F\n";
    out += "complete -c calx -n '__fish_seen_subcommand_from eval' -l release\n";
    for (const char* o : {"check", "nightly", "stable", "to", "rollback", "force"}) {
        out += std::string("complete -c calx -n '__fish_seen_subcommand_from update' -l ") + o + "\n";
    }
    out += "complete -c calx -n '__fish_seen_subcommand_from uninstall' -l yes\n";
    out += "complete -c calx -n '__fish_seen_subcommand_from serve' -a '(__fish_complete_directories)'\n";
    out += "complete -c calx -n '__fish_seen_subcommand_from serve' -s p -l port -r\n";
    out += "complete -c calx -n '__fish_seen_subcommand_from serve' -s b -l bind -r\n";
    out += std::string("complete -c calx -n '__fish_seen_subcommand_from completions' -a '") + kShells + "'\n";
    return out;
}

std::string powershell() {
    std::string commands;
    for (const Command& c : kCommands) commands += std::string("        '") + c.name + "' = '" + c.about + "'\n";
    return "# calx completions for PowerShell: add `calx completions powershell | Out-String | Invoke-Expression` to $PROFILE\n"
           "Register-ArgumentCompleter -Native -CommandName calx -ScriptBlock {\n"
           "    param($wordToComplete, $commandAst, $cursorPosition)\n"
           "    $commands = [ordered]@{\n" + commands + "    }\n"
           "    $words = @($commandAst.CommandElements | ForEach-Object { $_.ToString() })\n"
           "    $first = $words.Count -le 1 -or ($words.Count -eq 2 -and $wordToComplete -ne '')\n"
           "    $result = { param($text, $tip) [System.Management.Automation.CompletionResult]::new($text, $text, 'ParameterValue', $tip) }\n"
           "    $files = {\n"
           "        Get-ChildItem -Path \"$wordToComplete*\" -ErrorAction SilentlyContinue |\n"
           "            Where-Object { $_.PSIsContainer -or $_.Extension -eq '.calx' } |\n"
           "            ForEach-Object { $p = Resolve-Path -Relative $_.FullName; & $result $p $p }\n"
           "    }\n"
           "    if ($first) {\n"
           "        $commands.Keys | Where-Object { $_ -like \"$wordToComplete*\" } | ForEach-Object { & $result $_ $commands[$_] }\n"
           "        & $files\n"
           "        return\n"
           "    }\n"
           "    $options = switch ($words[1]) {\n"
           "        'update' { '" + std::string(kUpdateOptions) + "' }\n"
           "        'completions' { '" + std::string(kShells) + "' }\n"
           "        'uninstall' { '--yes' }\n"
           "        'serve' { if ($wordToComplete -like '-*') { '" + std::string(kServeOptions) + "' } else { $null } }\n"
           "        { $_ -in 'clean', 'doctor', 'version', 'help' } { '' }\n"
           "        default { if ($wordToComplete -like '-*') { '" + std::string(kBuildOptions) + "' } else { $null } }\n"
           "    }\n"
           "    if ($null -eq $options) { & $files; return }\n"
           "    $options -split ' ' | Where-Object { $_ -and $_ -like \"$wordToComplete*\" } | ForEach-Object { & $result $_ $_ }\n"
           "}\n";
}

// The program `calx new` starts: it greets its arguments, and has a test, so `calx NAME.calx`,
// `calx test NAME.calx` and `calx build NAME.calx` each do something the first time.
std::string starter(const std::string& name, bool script) {
    return std::string(script ? "#!/usr/bin/env calx\n" : "") + "// " + name +
           ": say here what it does.\n"
           "import std.env.{args}\n"
           "\n"
           "fn greeting(name: String) -> String {\n"
           "    return \"Hello, {name}!\"\n"
           "}\n"
           "\n"
           "fn main() {\n"
           "    let names = args()\n"
           "    if names.is_empty() {\n"
           "        println(greeting(\"world\"))\n"
           "    }\n"
           "    for name in names {\n"
           "        println(greeting(name))\n"
           "    }\n"
           "}\n"
           "\n"
           "// `calx test " + name + ".calx` runs each @test function in a process of its own.\n"
           "@test\n"
           "fn greets_by_name() {\n"
           "    assert(greeting(\"Ada\") == \"Hello, Ada!\")\n"
           "}\n";
}

// A program's name from a folder's or a file's: letters, digits, `-`, `_` and `.`, with anything
// else, which a shell would want quoted, as `-`. Bytes past ASCII are letters of some language.
std::string program_name(const std::string& from) {
    std::string out;
    for (char c : from) {
        unsigned char u = static_cast<unsigned char>(c);
        bool fine = u >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.';
        out += fine ? c : '-';
    }
    return out;
}

// A path as a shell takes it: in quotes if it has anything a shell would split or expand.
std::string shell_word(const std::string& path) {
    for (char c : path) {
        if (program_name(std::string(1, c)) == "-" && c != '-' && c != '/' && c != '\\' && c != ':') return "\"" + path + "\"";
    }
    return path;
}

bool write_new(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.close();
    if (!out) {
        std::fprintf(stderr, "calx new: couldn't write %s\n", path.string().c_str());
        return false;
    }
    return true;
}

}  // namespace

int run_new(const std::vector<std::string>& args) {
    if (args.size() != 1 || args[0].empty() || args[0][0] == '-') {
        std::fprintf(stderr,
                     "calx new: name the program: `calx new hello` makes hello/hello.calx, and `calx new tidy.calx` "
                     "one script\n");
        return 2;
    }
    std::string given = args[0];
    while (given.size() > 1 && (given.back() == '/' || given.back() == '\\')) given.pop_back();
    fs::path path(given);
    std::error_code ec;
    bool script = path.extension() == ".calx";
    fs::path dir = script ? path.parent_path() : path;
    // `.` and `..` have no name of their own, so the program takes the folder's.
    fs::path named = fs::absolute(script ? path : dir, ec).lexically_normal();
    while (!named.empty() && named.filename().empty()) named = named.parent_path();
    std::string name = program_name(script ? named.stem().string() : named.filename().string());
    if (name.empty() || name == "." || name == "..") {
        std::fprintf(stderr, "calx new: %s has no name for the program to take; give one, as `calx new hello`\n", given.c_str());
        return 2;
    }
    fs::path main = script ? path : dir / (name + ".calx");
    if (fs::exists(main, ec)) {
        std::fprintf(stderr, "calx new: %s is there already, so nothing was changed\n", main.string().c_str());
        return 1;
    }
    if (!dir.empty() && fs::exists(dir, ec) && !fs::is_directory(dir, ec)) {
        std::fprintf(stderr, "calx new: %s is a file, not a folder\n", dir.string().c_str());
        return 1;
    }
    if (!dir.empty()) {
        fs::create_directories(dir, ec);
        if (ec) {
            std::fprintf(stderr, "calx new: couldn't make %s: %s\n", dir.string().c_str(), ec.message().c_str());
            return 1;
        }
    }
    if (!write_new(main, starter(name, script))) return 1;
    // In a terminal, a ✓ before what it made, and the commands it gives in the accent.
    const term::Caps& c = term::out();
    std::string made = c.color ? term::tick(c) + " Made " : "Made ";
    auto say = [&](const std::string& text) { std::fputs(term::code(c, text, false).c_str(), stdout); };
    if (script) {
#ifndef _WIN32
        // So `./tidy.calx` runs it, through its `#!` line.
        fs::permissions(main, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
        std::string run = path.is_absolute() || given.rfind("./", 0) == 0 || given.rfind("../", 0) == 0 ? given : "./" + given;
        say(made + given + ". Run it with `calx " + shell_word(given) + "`, or `" + shell_word(run) + "`.\n");
#else
        say(made + given + ". Run it with `calx " + shell_word(given) + "`.\n");
#endif
        return 0;
    }
    fs::path ignore = dir / ".gitignore";
    bool kept = fs::exists(ignore, ec);
    if (!kept && !write_new(ignore, "# What `calx build` makes\n/" + name + "\n/" + name + ".exe\n/" + name + ".pdb\n")) return 1;
    bool here = fs::equivalent(dir, fs::current_path(ec), ec);
    if (c.color) {
        // What it made as a tree, then the commands to go on with, lined up with what each does.
        std::string root = here ? "." : given;
        std::printf("%s Made %s\n\n", term::tick(c).c_str(), term::bold(c, root).c_str());
        std::printf("    %s\n", term::accent(c, root + "/").c_str());
        std::printf("    %s %s%s\n", term::dim(c, "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80").c_str(), term::pad(name + ".calx", 14).c_str(),
                    term::dim(c, "reads its arguments, and has a test").c_str());
        std::printf("    %s %s%s\n\n", term::dim(c, "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80").c_str(), term::pad(".gitignore", 14).c_str(),
                    term::dim(c, kept ? "kept, as it was" : "leaves out what calx build makes").c_str());
        std::vector<std::pair<std::string, std::string>> next;
        if (!here) next.push_back({"cd " + shell_word(given), ""});
        next.push_back({"calx " + name + ".calx", "runs it"});
        next.push_back({"calx test " + name + ".calx", "runs its tests"});
        next.push_back({"calx build --release " + name + ".calx", "builds it"});
        std::size_t wide = 0;
        for (auto& [command, does] : next) wide = std::max(wide, command.size());
        std::printf("  %s\n", term::heading(c, "next").c_str());
        for (auto& [command, does] : next) {
            std::string shown = term::accent(c, command);
            if (!does.empty()) shown = term::pad(shown, static_cast<int>(wide) + 3) + term::dim(c, does);
            std::printf("    %s\n", shown.c_str());
        }
        return 0;
    }
    say(made + (here ? "a program here" : given) + ", with " + name + ".calx" +
        (kept ? ", and kept the .gitignore that was there" : " and a .gitignore") + ". Run it with:\n\n");
    if (!here) std::printf("    %s\n", term::accent(c, "cd " + shell_word(given)).c_str());
    std::printf("    %s\n\n", term::accent(c, "calx " + name + ".calx").c_str());
    say("`calx test " + name + ".calx` runs its tests, and `calx build --release " + name + ".calx` builds it.\n");
    return 0;
}

int run_completions(const std::vector<std::string>& args) {
    std::string shell = args.size() == 1 ? args[0] : "";
    std::string text;
    if (shell == "bash") {
        text = bash();
    } else if (shell == "zsh") {
        text = zsh();
    } else if (shell == "fish") {
        text = fish();
    } else if (shell == "powershell" || shell == "pwsh") {
        text = powershell();
    } else {
        std::fprintf(stderr, "calx completions: name the shell: bash, zsh, fish or powershell\n");
        return 2;
    }
#ifdef _WIN32
    // `\n` as it is, since bash in Git Bash or WSL reads a `\r` as part of the line.
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
}

}  // namespace calx
