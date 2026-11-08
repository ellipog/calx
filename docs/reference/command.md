---
title: The calx command
description: Starting, checking, building, running, testing and formatting Calx programs, running code from the command line, serving a folder, the build modes, and the object cache.
---

# The `calx` command

```sh
calx new hello                 # start a program: hello/hello.calx, with a test
calx check hello.calx          # check without building
calx run hello.calx            # build in debug mode and run
calx run hello.calx -- a b     # arguments after -- go to the program
calx hello.calx a b            # the same, as `python hello.py a b` is
calx build hello.calx -o hello # build an executable
calx build --release hello.calx
calx build --lib plugin.calx   # build a library, which a program opens while it runs
calx test tool.calx           # run its @test functions, each in a process of its own
calx eval "1 << 20"           # run code given on the command line, and print its value
calx fmt                       # format the .calx files here, in Calx's one format
calx serve                     # serve this folder's files at http://127.0.0.1:8000/
calx emit-c hello.calx         # print the C it compiles to
calx expand hello.calx         # print the code its attributes generate
calx version                   # print the version, the commit and the date
calx update                    # install the newest release
calx doctor                    # check that programs build and run, and say how to fix what doesn't
calx clean                     # remove the build cache
calx uninstall                 # remove Calx, its cache and its PATH entry
calx completions bash          # print completions for bash, zsh, fish or powershell
```

A program is its main file and the [modules](modules.md) it imports. The command takes the main file. A [library](libraries.md) is the same, without a `main`.

Before it checks a program, each command runs the [attributes](attributes.md) the program's libraries define, and adds the code they generate. Their answers are cached, so they run again only when a declaration with an attribute on it, or an attribute's code, changes. `calx expand` prints that code, without checking the program.

## Options

| Option | Meaning |
| --- | --- |
| `--release` | Optimize. Without it, programs build in debug mode, which builds fastest |
| `-o PATH` | Where `build` writes the executable, or `emit-c` the C: a file for one module, a folder for several. Folders on the way are made if they aren't there. `build` defaults to the file's name in the current folder |
| `--lib` | Build or check a [library](libraries.md): a `.dll` or `.so` with the functions marked `@export`, which a program opens while it runs. `calx run` doesn't take it |
| `--timings` | Print how long the front end, the C emitter, clang and the linker took |
| `--verbose` | Print the clang and linker commands |
| `--no-line` | Leave `#line` directives out of the C |
| `--json` | Print errors as JSON lines on standard output, [for editors](#errors-for-editors) |
| `--stdin` | Read the file's text from standard input, with the path naming it |

## Starting a program

```sh
calx new hello        # a folder hello, with hello.calx and a .gitignore
calx new tidy.calx    # one script
calx new .            # a program in this folder, named after it
```

`calx new hello` makes the folder `hello`, or uses it if it's there, and puts two files in it:

- `hello.calx`, a program that greets each of its arguments, or the world when it has none, with a `@test` for its one function. So `calx hello.calx Ada`, [`calx test hello.calx`](#testing) and `calx build hello.calx` each work at once, and the file shows the shape of a program to change into yours.
- `.gitignore`, which leaves out what `calx build` makes: `hello` on Linux, and `hello.exe` and `hello.pdb` on Windows. A `.gitignore` that's there is kept as it is.

The program is named after the folder, so `calx build` names the executable after it too. A character a shell would want quoted, such as a space, becomes `-` in the program's name: `calx new .` in a folder `my stuff` makes `my-stuff.calx`. `calx new` makes no repository, since the folder may be in one already; `git init` makes one.

A name ending in `.calx` makes just that file, starting with a `#!/usr/bin/env calx` line, and on Linux it's executable, so `./tidy.calx` runs it. Folders on the way are made.

`calx new` never overwrites: if the program's file is there, it says so, changes nothing, and exits with status 1.

## Running a file

`calx hello.calx a b` runs a program as `calx run hello.calx -- a b` does, the way `python hello.py a b` runs a script. Options for calx go before the file, and everything after it goes to the program untouched, so a program's own `--release` or `-o` reaches it:

```sh
calx tool.calx --release -o out.txt   # tool.calx gets --release, -o and out.txt
calx --release tool.calx input.txt    # built optimized; tool.calx gets input.txt
```

The program sees the file as its `argv[0]`, as a Python script sees `sys.argv[0]`, rather than the path of the program built from it, so [`std.env.program()`](../std/env.md) names it after the file, as `calx run` does too, and [`std.env.script()`](../std/env.md) gives its full path, as Python's `__file__` does. The options before the file are `--release`, `--timings` and `--verbose`. The file is one whose name ends in `.calx`, or any file that's there, for a script without the extension. A name that's neither a command nor a file is an error, with status 2, so `calx biuld x.calx` says so instead of running anything.

A first line that starts with `#!` is the system's, and the compiler skips it, so on Linux and macOS a program runs by its own name, as a shell or Python script does:

```sh
#!/usr/bin/env calx
```

```sh
chmod +x tool.calx
./tool.calx input.txt
```

`#!/usr/bin/env -S calx --release` builds it optimized. The program is built the first time and reused until it or the compiler changes, so a script starts in about the time its program takes to start, plus a few milliseconds for calx to check that nothing changed ([fast builds](#fast-builds)). Windows doesn't read `#!` lines: there, `calx tool.calx` runs it.

`calx run` stands for the program it runs. Its exit status is the program's, and on Windows, killing `calx run`, as a test runner stops one that ran too long, kills the program and every program it started, which `taskkill /T` can't reach once their parents have gone. A program that ends by itself leaves what it started running ([std.process](../std/process.md#when-this-program-ends)).

## Testing

`calx test` runs the tests in a file: its functions marked `@test`. Each test runs in a process of its own, as many at once as there are processors, so a panic or a crash fails only that test. They're reported in the order they're written, each as soon as those before it are done:

```calx
fn double(x: i32) -> i32 {
    return x * 2
}

@test
fn doubles() {
    assert(double(21) == 42)
}

@test
fn reads_a_number() ! String {
    let n = i32.parse("12") else {
        fail "not a number"
    }
    assert(double(n) == 24, "12 doubled is {double(n)}")
}

fn main() {
    println("{double(4)}")    // out: 8
}
```

```text
$ calx test tool.calx
ok    doubles
ok    reads_a_number
2 passed, 0 failed (0.02 s)
```

- A test takes nothing and returns nothing. It fails by panicking, which is what a failed `assert` does, or by failing with an error, as a `main` can: `fn reads_a_number() ! String`.
- What a test prints is shown only if it fails, under its name, with its panic or its error and where it happened.
- Names after the file run only the tests whose names hold one of them: `calx test tool.calx parse` runs `parses_ints` and `parse_errors`.
- `calx test` exits with 0 when every test passed, and 1 otherwise, or if the file has no tests.
- The file needn't have a `main`. If it does, `calx run` runs that, and `calx test` runs the tests instead. The tests are checked with the rest of the program, so a test that no longer compiles is an error under `calx run` too.
- The tests are the file's own. A test in a module the file imports runs when that module's file is tested.
- `--release` builds the tests optimized, for tests that do a lot of work.
- Each test is the built program run with a first argument naming it, which the runtime takes out, so `args()` in a test is empty. Run alone, the built program lists its tests.

## Running code from the command line

`calx eval` runs code given as one argument, as `python -c` does, and prints the value of its last statement, if it has one:

```sh
$ calx eval "1 << 20"
1048576
$ calx eval "let x = 3; x * x"
9
$ calx eval '"a,b;c".split(";")'
[a,b, c]
$ calx eval 'for i in 0..3 { print("{i} ") }'
0 1 2
$ calx eval "env.args()" -- one "two words"
[one, two words]
$ calx eval 'hex(hash.sha256("abc".to_bytes())); import std.bytes.{hex}'
ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
```

- The code is the body of a `main`. Statements are split at line breaks and at `;`s outside strings and brackets, and an `import` among them goes before the `main`, wherever it's written.
- A standard module the code names is imported for it: `math.`, `env.`, `hash.` or `json.` brings in `std.math`, `std.env`, `std.hash` or `std.json`. A function on its own, as `hex`, needs its `import`. A variable named as a module, as `let path = ...`, is the variable.
- The last statement is printed as `"{value}"` prints it. If it has no value, such as a loop or a call that returns nothing, it just runs.
- Arguments after `--` are the program's, which `env.args()` gives. `--release` before the code builds it optimized.
- An error names the line of the program `calx eval` built, which is shown with it. Like any program, the code is built once and reused while it's the same, so running it again starts at once.

## Formatting

```sh
calx fmt                    # format every .calx file in this folder and the folders in it
calx fmt tool.calx lib      # format tool.calx, and the .calx files under lib
calx fmt --check            # say which files aren't formatted, and change nothing
calx fmt --stdin < a.calx   # print standard input formatted, for editors
```

Calx has one format, with no settings, so code reads the same wherever it's from. `calx fmt`:

- indents four spaces for each bracket a line is inside, counting the brackets one line opens once, and once more for a line that goes on with the statement before it, after an operator such as `+`, `and` or `=`. A line that starts by closing a bracket lines up with the line that opened it;
- puts one space after `,`, `;` and `:`, and around `=`, `->`, `=>` and binary operators, and none inside `()` and `[]`, before `,` and `:`, or around `.`. Braces get a space inside, as in `Point { x: 1 }`, except `{}` and an import's list, `std.fs.{read, write}`;
- spaces `a < b` but not `List<i32>`, from what the parser read each `<` and `>` as. Where the space is part of the meaning, as before a `(` that calls or a `?` that makes a type optional, it stays as it was;
- keeps line breaks where they are, since a line break ends a statement, and keeps at most one blank line in a row, with none just inside a block or at the ends of the file;
- keeps comments as they're written, apart from spaces at their ends, and a comment after code keeps its column, so comments that line up stay lined up;
- ends each line with `\n`, with no spaces before it, and the file with one.

It doesn't wrap long lines or join short ones, and a string that spans lines keeps its lines, since they're its text.

It checks what it writes: the result must have the same tokens and comments, with the same statement ends and operators, and formatting it again must change nothing. A file that doesn't parse, or that fails the check, is left as it was, with the error on standard error, and `calx fmt` exits with status 1. `--check` exits with status 1 when a file isn't formatted, for CI. Folders whose names start with `.` are skipped. Format Document in VS Code runs it too ([editor support](editors.md)).

## Serving a folder

`calx serve` serves a folder's files over HTTP, as `python -m http.server` does, until Ctrl+C stops it:

```sh
calx serve                  # this folder, at http://127.0.0.1:8000/
calx serve site             # the folder site
calx serve --port 9000      # on port 9000; 0 lets the system pick one
calx serve --bind ""        # on every address, so other machines can reach it
```

```text
$ calx serve site
Serving HTTP on 127.0.0.1 port 8000 (http://127.0.0.1:8000/) ...
127.0.0.1 - - [08/Nov/2026 12:55:25] "GET / HTTP/1.1" 200 -
127.0.0.1 - - [08/Nov/2026 12:55:25] "GET /style.css HTTP/1.1" 200 -
```

- A folder's `index.html` or `index.htm` is served for it, and a folder without one gets a page listing what's in it. The rest is as [std.http.server](../std/http-server.md#serving-a-folder)'s `serve_dir` serves it, which is what `calx serve` runs.
- It listens on `127.0.0.1` unless `--bind` (`-b`) gives another address, so only this machine reaches it, and Windows' firewall doesn't ask about it. Python listens on every address unless it's told otherwise.
- `--port` (`-p`) gives the port. Without it, `calx serve` takes 8000, or the next free one up to 8010 if 8000 is taken, and says which on its first line. A port that's given and taken is an error.
- Each request is logged to standard error, as Python logs it.
- The program that serves is built the first time, optimized, and reused after that, so it starts at once.
- `calx serve` exits with status 2 for arguments it can't use, and 1 for a folder that isn't there or a port it can't listen on.

## Exit status

| Status | When |
| --- | --- |
| 0 | Success |
| 1 | The program has errors, or building it failed |
| 2 | The command line is wrong, such as a name that's neither a command nor a file to run |
| 1 | `calx test`: a test failed, no test's name held one of the names given, or the file has no tests |
| *the code's* | `calx eval` exits with the status of the program its code became, or 1 if the code has errors |
| *the program's* | `calx run`, and `calx file.calx`, exit with the program's status: 0, 101 after a [panic](printing.md#panics), or 102 when a debug build finds memory that was never freed |

## Build modes

Both modes keep every check: bounds checks, division by zero, and Java's integer and float semantics. They differ in how much clang optimizes, and in one check of the compiler itself.

| Mode | clang flags | For |
| --- | --- | --- |
| debug (the default) | `-O0 -DCX_DEBUG` | Fast builds while you work |
| release | `-O3 -march=x86-64-v3 -flto=thin` | Speed |

Release builds use `-O3`, the level Rust's release builds use, since it's the one that specializes a loop for each value of a test that doesn't change inside it, such as which layout a palette has (D89). In both modes the C is compiled with `-std=gnu17 -fwrapv -ffp-contract=off -fno-math-errno -gline-tables-only`. `-fwrapv` and `-ffp-contract=off` are part of Calx's semantics: without them C wouldn't wrap on overflow or round every float operation as Java does ([integers](integers.md), [floats](floats.md)).

Release builds also optimize the program as a whole when it links, with ThinLTO (D99). Each module is a C file of its own, so without it a call from one module to another could never be inlined, however small the function: a loop that adds vectors from another module ran 4.6 times faster with it, and one that packs block positions 7.6 times. The runtime's functions inline the same way. lld does this, so it needs lld next to clang or on `PATH`; with another linker, each file is optimized alone. Since clang can then see that a call to a function in another module has no side effects, a benchmark should pass what it measures through [`std.hint.black_box`](../std/hint.md).

In debug builds the runtime checks memory: a block freed twice or with the wrong size stops the program, and a program that ends with blocks still allocated says so. Either way it exits with status 102. The compiler frees every value exactly once ([ownership](ownership.md#in-c)), so that's a bug in Calx, worth reporting. Release builds don't check, and use Calx's own allocator, which is faster than the system's.

Both modes have line tables, which don't change the generated code, so a debugger or profiler shows Calx source lines. On Windows they're in a PDB next to the executable in the cache, which the executable records.

On Windows, debug builds use the C runtime DLL that comes with Visual Studio, since linking the static one puts its debug information into every PDB and doubles the link time. Release builds link the C runtime statically, so they run on any Windows machine.

## Fast builds

Building should take moments, so the command avoids every step it can:

- The front end checks the whole program every time. It's fast enough that tracking what to skip would cost more.
- **One C file per module.** Each holds exactly what its module uses, so its text changes only when that does ([modules](modules.md#in-c)). The files that changed compile in parallel.
- **The object cache.** clang's output is cached under a hash of the C text, the flags and the clang executable. When a file's C is unchanged, clang doesn't run for it.
- **The runtime is compiled once** per build mode and clang version, then reused from the cache.
- When nothing changed at all, `calx run` reuses the linked program and only starts it.
- Programs link with lld when it's installed next to clang or on `PATH`.
- **Release links reuse their work.** lld keeps the machine code ThinLTO made for each file in a cache of its own, so after an edit it compiles again only the files the edit could change. A release build of a 64-module program takes 0.26 s after an edit to one module, against 0.89 s from an empty cache.
- **The linker runs directly.** clang's driver only works out the linker's command line, and starting clang costs about 18 ms. So the first link asks clang for that command once, and caches it. If a cached command fails, say after a Visual Studio update, clang works it out again.

Measured on the reference machine (an i9-13900K) with a small program, in debug mode:

| | Windows 11 | Linux (WSL 2) |
| --- | --- | --- |
| `calx run` with nothing changed | 13 ms | 13 ms |
| `calx check` | 6 ms | 10 ms |
| `calx run` after an edit | 110 ms, of which 70 ms is Windows Defender | 40 ms |
| `calx run` with a cold cache, compiling the runtime too | 250 ms | 125 ms |

Windows Defender scans every new executable the first time it starts, which costs about 70 ms and comes after the build. An exclusion for the cache folder removes that, at the cost of Defender no longer scanning what's built there.

## Environment

| Variable | Meaning |
| --- | --- |
| `CALX_CC` | The clang to use. Otherwise `clang` is found on `PATH` |
| `CALX_HOME` | Where the runtime and the [standard library](modules.md#the-standard-library) are. Otherwise, the installation `calx` is in: the folder above its `bin`, which holds `runtime` and `std`. A compiler built from source that isn't in one uses the checkout it was built from |
| `CALX_CACHE` | The object cache. Otherwise `%LOCALAPPDATA%\calx\cache` on Windows, and `$XDG_CACHE_HOME/calx` or `~/.cache/calx` on Linux |
| `CALX_NO_UPDATE_CHECK` | Set to anything, turns off the [daily check](#updating) for a newer release |
| `CALX_PROFILE` | Set to `1`, builds keep every function's frame pointer, so a sampling profiler such as `perf record -g` sees the calls that led to each sample. It costs a register, a few percent of speed, so it's off otherwise |
| `CALX_RELEASES` | Where `calx update` finds releases. Otherwise `https://github.com/ellipog/calx/releases`. For a mirror, or a test |

`calx run` sets two for the program it runs. `CALX` is the path of the `calx` that runs it, as Python's `sys.executable` is, so a script that builds or checks other programs, such as [the test suite](../../tests/run.calx), uses the same compiler, and so do the programs it starts. `CALX_SCRIPT` is the full path of the file, which [`std.env.script()`](../std/env.md) gives; the program's runtime takes it out of the environment as it starts, so a program it starts doesn't see it.

The cache keeps the 64 programs and 1024 compiled files used most recently, and at most 256 MB of each, and deletes the rest after each build. It keeps the code attributes generated for the 256 programs that used it most recently. Anything used in the last 10 minutes stays, past those limits, since a build running beside this one may need it. lld keeps at most 512 MB of release builds' machine code in its `lto` folder. Deleting the cache is always safe.

`calx build` copies what it built out of the cache. On Windows, the program's line tables are in a PDB, which goes beside it with the same name, as `hello.pdb`, so stack traces and debuggers still find them after the cache lets the program go. On Linux they're in the executable itself.

## Checking the toolchain

`calx doctor` checks what building programs needs, and for each problem says how to fix it:

```
calx doctor

  ok    calx 0.2.0 (3f9c2a1b7d4e, 2026-11-01) at C:\Users\you\.calx\bin\calx.exe
  ok    runtime and standard library in C:\Users\you\.calx
  ok    clang 20.1.8 at C:\Program Files\LLVM\bin\clang.exe
  ok    lld, which links faster and optimizes across modules in release builds
  ok    builds and runs a program in a debug build (143 ms)
  ok    builds and runs a program in a release build (260 ms)
  ok    calx on PATH is this one
  ok    build cache in C:\Users\you\AppData\Local\calx\cache, 412 MB
  ok    updates: `calx update` installs new releases, and a daily check says when there's one

Everything works.
```

The real test is building a program, in debug and release, which is what everything else is for. When that fails, it shows what clang said and names what's missing: on Windows, the C runtime and SDK from Visual Studio's Build Tools; on Linux, the C library's headers. It exits with 1 if something needs fixing, and 0 if not. A failed build points at it too.

## Updating

An installed Calx updates itself from the releases on [GitHub](https://github.com/ellipog/calx/releases):

```sh
calx update                # install the newest release on this one's channel
calx update --check        # say whether there's a newer one, and change nothing
calx update --nightly      # follow the nightly releases from now on
calx update --stable       # follow the stable releases from now on
calx update --to 0.2.0     # install a version, newer or older
calx update --rollback     # put back what the last update replaced; again, to undo that
```

- **Channels.** Stable releases are tagged `vX.Y.Z`. A nightly is published for every push to `main` that passes CI, with a version such as `0.2.0-nightly.202611011530`, a prerelease of the next stable version. `calx update` stays on the channel of the version that's running. Until there's a first stable release, the stable channel follows the nightlies.
- **Each download is checked** against the SHA-256 the release publishes beside it, and nothing changes if it doesn't match. Releases come only from github.com/ellipog/calx, over HTTPS, so the trust is in that account and TLS. They aren't signed.
- **The swap is quick and can't leave half an installation.** The new release is unpacked beside the installation, then each part is renamed into place, with what it replaces renamed into `.update/old`. If a rename fails, the ones before it are put back. On Windows, the running `calx.exe` is renamed out of the way, which Windows allows, where overwriting it isn't.
- **A build from source** says to update with `git pull` and a rebuild, and how to install a release instead.

`calx update` runs `calx-update`, a Calx program that ships beside `calx` in each release: [tools/update/update.calx](../../tools/update/update.calx). It's built on [`std.http`](../std/http.md), [`std.hash`](../std/hash.md), [`std.zip`](../std/zip.md) and [`std.tar`](../std/tar.md).

### Cleaning, uninstalling and completions

```sh
calx clean                 # remove the build cache, and say how much that freed
calx uninstall             # remove Calx, after asking
calx uninstall --yes       # the same, without asking, for a script
calx completions bash      # print the completions for a shell
```

- **`calx clean`** removes the [cache](#fast-builds): every compiled object and program, what attributes answered, and what the daily check found. The next build makes what it needs again, so it's always safe, though a build running at the same time may fail and need running again. On Windows a program that's running holds its file, which is kept, and `calx clean` says how many were.
- **`calx uninstall`** removes what the install script made: the installation folder, its build cache, and its `bin` from your PATH. On Windows that's your user Path in the registry, and on Linux the `# Calx` lines the script added to `~/.profile`, `~/.bashrc` and `~/.zshrc`. It says what it will remove and asks first, unless given `--yes`; without a terminal to ask in, it needs `--yes`. `CALX_NO_MODIFY_PATH=1` leaves PATH alone, as it does for the install script. On Windows the folder is removed just after the command ends, since a running `calx.exe` can't remove itself. A Calx built from source has nothing to uninstall, and says so.
- **`calx completions SHELL`** prints a completion script for `bash`, `zsh`, `fish` or `powershell`: the commands with what each does, their options, and `.calx` files. The script's first line says where it goes:

| Shell | Add |
| --- | --- |
| bash | `eval "$(calx completions bash)"` to `~/.bashrc` |
| zsh | `eval "$(calx completions zsh)"` to `~/.zshrc`, after `compinit` |
| fish | `calx completions fish > ~/.config/fish/completions/calx.fish`, once |
| PowerShell | `calx completions powershell \| Out-String \| Invoke-Expression` to `$PROFILE` |

### The daily check

At most once a day, a command starts `calx-update` in the background to look for a newer release, and goes on without waiting. Each command reads what the last check found from one small file in the cache. When a newer release is out, a command says so on standard error, at most once a day:

```
calx 0.2.0 is out; you have 0.1.0. `calx update` installs it.
```

Nothing is checked or said when standard error isn't a terminal, with `--json`, in CI (when `CI` is set), with `CALX_NO_UPDATE_CHECK` set, or in a build from source. A failed check, such as one offline, says nothing, and waits a day like any other.

## Errors

Errors are printed in source order, in the form `path:line:column: error: message`, followed by the line and a caret under the problem, the way clang prints them. A note can follow, pointing at a related place. There are no warnings: everything the compiler objects to is an error (D63).

## Errors for editors

`--json` prints the errors on standard output instead, one JSON object per line, for editors and other tools. `--stdin` reads the file's text from standard input, so an editor can check text it hasn't saved. The path on the command line still names the file in errors, and its folder is where imports will be found.

```sh
calx check --json --stdin src/main.calx < unsaved-text
```

```json
{"severity":"error","file":"src/main.calx","range":{"start":{"line":2,"character":14},"end":{"line":2,"character":15}},"message":"unknown name `x`","notes":[]}
```

| Field | Meaning |
| --- | --- |
| `severity` | `error`, or `warning` for a deprecation |
| `file` | The path as the compiler was given it, or `null` for an error that isn't about a place in a file |
| `range` | Where the problem is. Lines and characters count from 0, and characters are UTF-16 code units, as editors and the Language Server Protocol count them. An empty span becomes the one character it's at, so there's always something to underline |
| `message` | The message, as the text form prints it |
| `notes` | Related places, each with its own `file`, `range` and `message` |

The status is 1 when there are errors, as without `--json`. The test suite checks that every error test reports the same errors at the same places in both forms. [Editor support](editors.md) uses this format.
