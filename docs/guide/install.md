---
title: Install Calx
description: Build the calx command from source on Windows or Linux, put it on your PATH, and check that it runs a program.
---

# Install Calx

Calx has no releases yet, so you build the compiler from source. It's one CMake build. Calx runs on Windows and Linux, on 64-bit x86.

## What you need

- **Git**, to get the source.
- **CMake** 3.20 or later, and **Ninja**.
- **clang and lld**, from LLVM 18 or later. Calx compiles programs to C and has clang build that C, so you need clang to run programs, as well as to build the compiler.
- **On Windows, Visual Studio or its Build Tools**, with the "Desktop development with C++" workload. clang links programs against the C runtime and the Windows SDK that come with it. You can get them from [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/).
- **Python 3**, only if you want to run the test suite.

On Ubuntu or Debian:

```sh
sudo apt install git cmake ninja-build clang lld
```

If `clang --version` says your clang is older than 18, [apt.llvm.org](https://apt.llvm.org/) has newer ones.

On Windows, with winget:

```sh
winget install Git.Git
winget install Kitware.CMake
winget install Ninja-build.Ninja
winget install LLVM.LLVM
```

LLVM's installer puts clang in `C:\Program Files\LLVM\bin`. Add that folder to your PATH, or set `CALX_CC` to the full path of `clang.exe`.

Release builds use x86-64-v3 instructions, so the programs they make need a processor with AVX2, as most made since 2015 have. Debug builds run on any 64-bit x86 processor.

## Build the compiler

```sh
git clone https://github.com/ellipog/calx
cd calx
cmake -S compiler -B build/compiler -G Ninja -DCMAKE_CXX_COMPILER=clang++
ninja -C build/compiler
```

That makes `build/compiler/calx`, or `calx.exe` on Windows. It finds its runtime in the checkout it was built from, so leave the checkout where it is. If you move it, set `CALX_HOME` to the new place.

Add the `build/compiler` folder to your PATH. The guide and the reference call the command `calx`.

## Check that it works

From the checkout:

```sh
calx run tests/basics/hello.calx
```

It prints `hello, calx`, then four more lines. The first run builds Calx's runtime as well as the program, which takes a moment. After that, a program that hasn't changed starts in milliseconds.

If it says it can't find clang, put clang on your PATH, or set `CALX_CC` to its full path.

## Where builds go

The compiler caches what it builds. The cache is in `%LOCALAPPDATA%\calx\cache` on Windows, and in `~/.cache/calx` on Linux, or under `XDG_CACHE_HOME` if that's set. `CALX_CACHE` puts it somewhere else. Deleting it is always safe: the next build makes it again.

## The tests

`python tests/run.py` builds the compiler and runs the test suite, which includes every example in these docs. CI runs it in debug and release builds, on Windows and Linux.

## Next

- Set up your editor: the VS Code extension highlights Calx and shows errors as you type ([editor support](../reference/editors.md)).
- Start the guide with [Getting started](getting-started.md), which writes a first program.
