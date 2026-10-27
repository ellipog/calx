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

For now a program is one file, and that file is its module.

## Options

| Option | Meaning |
| --- | --- |
| `--release` | Optimize. Without it, programs build in debug mode, which builds fastest |
| `-o PATH` | Where `build` writes the executable, or `emit-c` the C. `build` defaults to the file's name in the current folder |
| `--timings` | Print how long the front end, the C emitter, clang and the linker took |
| `--verbose` | Print the clang commands |
| `--no-line` | Leave `#line` directives out of the C |

## Exit status

| Status | When |
| --- | --- |
| 0 | Success |
| 1 | The program has errors, or building it failed |
| 2 | The command line is wrong |
| *the program's* | `calx run` exits with the program's status: 0, or 101 after a [panic](printing.md#panics) |

## Build modes

Both modes keep every check: bounds checks, division by zero, and Java's integer and float semantics. They differ only in how much clang optimizes.

| Mode | clang flags | For |
| --- | --- | --- |
| debug (the default) | `-O0` | Fast builds while you work |
| release | `-O2 -march=x86-64-v3` | Speed |

In both modes the C is compiled with `-std=gnu17 -fwrapv -ffp-contract=off -fno-math-errno`. `-fwrapv` and `-ffp-contract=off` are part of Calx's semantics: without them C wouldn't wrap on overflow or round every float operation as Java does ([integers](integers.md), [floats](floats.md)).

## Fast builds (D64)

Building should take moments, so the command avoids every step it can:

- The front end checks the whole program every time. It's fast enough that tracking what to skip would cost more.
- **The object cache.** clang's output is cached under a hash of the C text, the flags and the clang executable. When the C is unchanged, clang doesn't run.
- **The runtime is compiled once** per build mode and clang version, then reused from the cache.
- When nothing changed at all, `calx run` reuses the linked program and only starts it.
- Programs link with lld when it's installed next to clang or on `PATH`.

Measured on the reference machine (an i9-13900K on Windows 11) with a small program:

| | Time |
| --- | --- |
| `calx run` with nothing changed | 15 ms |
| `calx check` | 6 ms |
| `calx run` with a cold cache, compiling the runtime too | about 230 ms |

## Environment

| Variable | Meaning |
| --- | --- |
| `CALX_CC` | The clang to use. Otherwise `clang` is found on `PATH` |
| `CALX_HOME` | Where the runtime is. Otherwise, the source tree the compiler was built from |
| `CALX_CACHE` | The object cache. Otherwise `%LOCALAPPDATA%\calx\cache` on Windows, and `$XDG_CACHE_HOME/calx` or `~/.cache/calx` on Linux |

The cache only ever grows. Deleting it is always safe.

## Errors

Errors are printed in source order, in the form `path:line:column: error: message`, followed by the line and a caret under the problem, the way clang prints them. A note can follow, pointing at a related place. There are no warnings: everything the compiler objects to is an error (D63).
