---
title: Install Calx
description: Install Calx with one command on Windows or Linux, keep it up to date with calx update, or build it from source.
---

# Install Calx

One command installs Calx. It runs on Windows and Linux, on 64-bit x86.

On Windows, in PowerShell:

```powershell
irm https://calxlang.com/install.ps1 | iex
```

On Linux:

```sh
curl -fsSL https://calxlang.com/install.sh | sh
```

Each downloads the newest release from [GitHub](https://github.com/ellipog/calx/releases), checks it against the SHA-256 published with it, unpacks it into `.calx` in your home folder, and puts `.calx/bin` on your PATH. Neither needs administrator rights or root. Open a new terminal afterwards, so it sees the new PATH. Running one again reinstalls.

Calx is pre-release, so until the first stable release, the scripts install the newest nightly: the newest build of `main` that passed every test.

| Variable | Does |
| --- | --- |
| `CALX_INSTALL` | Installs somewhere else than `~/.calx` |
| `CALX_CHANNEL=nightly` | Installs the newest nightly even once there are stable releases |
| `CALX_NO_MODIFY_PATH=1` | Leaves PATH alone |

## clang

Calx compiles programs to C and has clang build that C, so programs need **clang and lld, from LLVM 18 or later**. Releases will soon carry their own ([CLX-110](https://acta.aaenz.no/aaen-studios/clx#CLX-110)). Until then, the install scripts say if clang is missing.

On Ubuntu or Debian:

```sh
sudo apt install clang lld
```

If `clang --version` says your clang is older than 18, [apt.llvm.org](https://apt.llvm.org/) has newer ones.

On Windows:

```powershell
winget install LLVM.LLVM
```

clang links Windows programs with the C runtime and the Windows SDK from **Visual Studio's Build Tools**, with the "Desktop development with C++" workload, from [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/). LLVM's installer puts clang in `C:\Program Files\LLVM\bin`. Add that folder to your PATH, or set `CALX_CC` to the full path of `clang.exe`.

Release builds use x86-64-v3 instructions, so the programs they make need a processor with AVX2, as most made since 2015 have. Debug builds run on any 64-bit x86 processor.

## Check that it works

Save this as `hello.calx`:

```calx
fn main() {
    println("hello, calx")    // out: hello, calx
}
```

and run it:

```sh
calx run hello.calx
```

The first run builds Calx's runtime as well as the program, which takes a moment. After that, a program that hasn't changed starts in milliseconds.

If it says it can't find clang, put clang on your PATH, or set `CALX_CC` to its full path.

## Staying up to date

```sh
calx update
```

installs the newest release, and `calx update --rollback` goes back. Once a day, a `calx` command checks for a newer release in the background and says when there is one. [The calx command](../reference/command.md#updating) has the details, and how to turn the check off.

## Where builds go

The compiler caches what it builds. The cache is in `%LOCALAPPDATA%\calx\cache` on Windows, and in `~/.cache/calx` on Linux, or under `XDG_CACHE_HOME` if that's set. `CALX_CACHE` puts it somewhere else. Deleting it is always safe: the next build makes it again.

## Building from source

To work on Calx itself, build the compiler from a checkout. As well as clang and lld, you need **Git**, **CMake** 3.20 or later and **Ninja**, and **Python 3** to run the test suite.

```sh
git clone https://github.com/ellipog/calx
cd calx
cmake -S compiler -B build/compiler -G Ninja -DCMAKE_CXX_COMPILER=clang++
ninja -C build/compiler
```

On Ubuntu or Debian, `sudo apt install git cmake ninja-build clang lld` gets the tools; on Windows, `winget install Git.Git Kitware.CMake Ninja-build.Ninja LLVM.LLVM`.

That makes `build/compiler/calx`, or `calx.exe` on Windows. It finds its runtime in the checkout it was built from, so leave the checkout where it is. If you move it, set `CALX_HOME` to the new place. `calx version` says `0.1.0-dev (built from source)`, where a release names its commit and date. `calx update` doesn't update a build from source: `git pull` and `ninja` do.

`build/compiler/calx run tests/run.calx` runs the test suite with that compiler, and it includes every example in these docs. CI runs it in debug and release builds, on Windows and Linux.

## Next

- Set up your editor: the VS Code extension highlights Calx and shows errors as you type ([editor support](../reference/editors.md)).
- Start the guide with [Getting started](getting-started.md), which writes a first program.
