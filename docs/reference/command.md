---
title: The calx command
description: Checking, building and running Calx programs, the build modes, and the object cache.
---

# The `calx` command

```sh
calx check hello.calx          # check without building
calx run hello.calx            # build in debug mode and run
calx run hello.calx -- a b     # arguments after -- go to the program
calx build hello.calx -o hello # build an executable
calx build --release hello.calx
calx emit-c hello.calx         # print the C it compiles to
calx version
```

A program is its main file and the [modules](modules.md) it imports. The command takes the main file.

## Options

| Option | Meaning |
| --- | --- |
| `--release` | Optimize. Without it, programs build in debug mode, which builds fastest |
| `-o PATH` | Where `build` writes the executable, or `emit-c` the C: a file for one module, a folder for several. `build` defaults to the file's name in the current folder |
| `--timings` | Print how long the front end, the C emitter, clang and the linker took |
| `--verbose` | Print the clang and linker commands |
| `--no-line` | Leave `#line` directives out of the C |
| `--json` | Print errors as JSON lines on standard output, [for editors](#errors-for-editors) |
| `--stdin` | Read the file's text from standard input, with the path naming it |

## Exit status

| Status | When |
| --- | --- |
| 0 | Success |
| 1 | The program has errors, or building it failed |
| 2 | The command line is wrong |
| *the program's* | `calx run` exits with the program's status: 0, 101 after a [panic](printing.md#panics), or 102 when a debug build finds memory that was never freed |

## Build modes

Both modes keep every check: bounds checks, division by zero, and Java's integer and float semantics. They differ in how much clang optimizes, and in one check of the compiler itself.

| Mode | clang flags | For |
| --- | --- | --- |
| debug (the default) | `-O0 -DCX_DEBUG` | Fast builds while you work |
| release | `-O3 -march=x86-64-v3` | Speed |

Release builds use `-O3`, the level Rust's release builds use, since it's the one that specializes a loop for each value of a test that doesn't change inside it, such as which layout a palette has (D89). In both modes the C is compiled with `-std=gnu17 -fwrapv -ffp-contract=off -fno-math-errno -gline-tables-only`. `-fwrapv` and `-ffp-contract=off` are part of Calx's semantics: without them C wouldn't wrap on overflow or round every float operation as Java does ([integers](integers.md), [floats](floats.md)).

In debug builds the runtime checks memory: a block freed twice or with the wrong size stops the program, and a program that ends with blocks still allocated says so. Either way it exits with status 102. The compiler frees every value exactly once ([ownership](ownership.md#in-c)), so that's a bug in Calx, worth reporting. Release builds don't check, and use Calx's own allocator, which is faster than the system's.

Both modes have line tables, which don't change the generated code, so a debugger or profiler shows Calx source lines. On Windows they're in a PDB next to the executable in the cache, which the executable records.

On Windows, debug builds use the C runtime DLL that comes with Visual Studio, since linking the static one puts its debug information into every PDB and doubles the link time. Release builds link the C runtime statically, so they run on any Windows machine.

## Fast builds (D64)

Building should take moments, so the command avoids every step it can:

- The front end checks the whole program every time. It's fast enough that tracking what to skip would cost more.
- **One C file per module.** Each holds exactly what its module uses, so its text changes only when that does ([modules](modules.md#in-c-d75)). The files that changed compile in parallel.
- **The object cache.** clang's output is cached under a hash of the C text, the flags and the clang executable. When a file's C is unchanged, clang doesn't run for it.
- **The runtime is compiled once** per build mode and clang version, then reused from the cache.
- When nothing changed at all, `calx run` reuses the linked program and only starts it.
- Programs link with lld when it's installed next to clang or on `PATH`.
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
| `CALX_HOME` | Where the runtime and the [standard library](modules.md#the-standard-library) are. Otherwise, the source tree the compiler was built from |
| `CALX_CACHE` | The object cache. Otherwise `%LOCALAPPDATA%\calx\cache` on Windows, and `$XDG_CACHE_HOME/calx` or `~/.cache/calx` on Linux |

The cache keeps the 64 programs and 1024 compiled files used most recently, and at most 256 MB of each, and deletes the rest after each build. Deleting it is always safe.

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
