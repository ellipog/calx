---
title: Install Calx
description: Install Calx with one command on Windows or Linux, keep it up to date with calx update, add completions, remove it with calx uninstall, or build it from source.
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

Each downloads the newest release from [GitHub](https://github.com/ellipog/calx/releases), and the [toolchain](#clang) it builds programs with, checks them against the SHA-256s published with them, unpacks them into `.calx` in your home folder, and puts `.calx/bin` on your PATH. Neither needs administrator rights or root. Open a new terminal afterwards, so it sees the new PATH. Running one again reinstalls, and downloads the toolchain again only if the release builds with a different one.

In a terminal, Calx's mark dissolves in, each download draws a bar with how much has come and how fast, and a scanner sweeps while the toolchain unpacks. The script then builds and runs a first program, which checks that everything works and compiles Calx's runtime, so your first program builds as quickly as the rest. Into a pipe or a file, in CI, or with `NO_COLOR` set, the scripts print plain lines, one a step, and leave that first build to your first program ([in a terminal](../reference/command.md#in-a-terminal)).

<!-- terminal: install -->

On Linux, the script needs `curl` or `wget`, `sha256sum`, and `tar` with `xz` for the toolchain, which most systems have. It says how to install the C library's headers if they're missing ([clang](#clang)), and libcurl, which `calx update` downloads with: on Ubuntu or Debian, `sudo apt install libcurl4`.

Calx is pre-release, so until the first stable release, the scripts install the newest nightly: the newest build of `main` that passed every test.

| Variable | Does |
| --- | --- |
| `CALX_INSTALL` | Installs somewhere else than `~/.calx` |
| `CALX_CHANNEL=nightly` | Installs the newest nightly even once there are stable releases |
| `CALX_NO_MODIFY_PATH=1` | Leaves PATH alone |

## clang

Calx compiles programs to C and has clang build that C. A release brings its own, in `.calx/toolchain`: clang and lld from LLVM 23, so nothing else needs installing, not LLVM, and on Windows not Visual Studio.

- **On Windows**, with [mingw-w64](https://www.mingw-w64.org/)'s headers and libraries for the Windows C runtime, the UCRT, which every Windows from 10 on has. The programs it builds need only Windows 10 or later. It's a 60 MB download.
- **On Linux**, programs use the system's C library, glibc 2.35 or later, with its headers and start files, and compiler-rt in place of GCC's parts, so GCC needn't be installed. It's a 90 MB download. Most systems have glibc's headers; the install script says if they're missing, which on Ubuntu or Debian is `sudo apt install libc6-dev`, and on Fedora `sudo dnf install glibc-devel`.

The toolchain is a download of its own, which changes only with LLVM, so most updates leave it as it is. `calx update --toolchain` installs it again if it goes missing, and `calx doctor` says if it is. `CALX_CC` picks another clang, such as one from LLVM's installer; on Windows that one builds with Visual Studio's C runtime and SDK, from its Build Tools with the "Desktop development with C++" workload.

Release builds use x86-64-v3 instructions, so the programs they make need a processor with AVX2, as most made since 2015 have. Debug builds run on any 64-bit x86 processor. On Arm64, release builds count on ARMv8.2, which the Raspberry Pi 5, AWS's Graviton 2 and later, Ampere's processors and every Snapdragon laptop have, and debug builds run on any Arm64 processor, the Raspberry Pi 4's too. `calx doctor` says which a build counts on.

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

Unless the install script's first program already did it, the first run builds Calx's runtime as well as the program, which takes a moment. After that, a program that hasn't changed starts in milliseconds.

If it says it can't find clang, `calx update --toolchain` installs the toolchain again. `calx doctor` checks everything a build needs and says how to fix what's wrong.

`calx new hello` starts a program of your own: a folder `hello` with `hello.calx`, which reads its arguments and has a test, and a `.gitignore` ([starting a program](../reference/command.md#starting-a-program)).

## Staying up to date

```sh
calx update
```

installs the newest release, and `calx update --rollback` goes back. Once a day, a `calx` command checks for a newer release in the background and says when there is one. [The calx command](../reference/command.md#updating) has the details, and how to turn the check off.

## Completions

`calx completions` prints a script that completes `calx`'s commands, options and `.calx` files as you type. In PowerShell, add this line to your `$PROFILE`:

```powershell
calx completions powershell | Out-String | Invoke-Expression
```

In bash, add `eval "$(calx completions bash)"` to `~/.bashrc`. zsh and fish are in [the calx command](../reference/command.md#cleaning-uninstalling-and-completions).

## Where builds go

The compiler caches what it builds. The cache is in `%LOCALAPPDATA%\calx\cache` on Windows, and in `~/.cache/calx` on Linux, or under `XDG_CACHE_HOME` if that's set. `CALX_CACHE` puts it somewhere else. Deleting it is always safe: the next build makes it again. `calx clean` deletes it and says how much room that gave back.

## Uninstalling

```sh
calx uninstall
```

removes Calx: its folder, with the toolchain it carries, its build cache, and the PATH entry the install script added. It asks before it removes anything. A clang installed on its own stays, since other programs may use it.

## Building from source

To work on Calx itself, build the compiler from a checkout. As well as clang and lld, you need **Git**, **CMake** 3.20 or later and **Ninja**. The test suite is a Calx program, so it needs nothing more. It builds the same way on x64 and on Arm64.

```sh
git clone https://github.com/ellipog/calx
cd calx
cmake -S compiler -B build/compiler -G Ninja -DCMAKE_CXX_COMPILER=clang++
ninja -C build/compiler
```

On Ubuntu or Debian, `sudo apt install git cmake ninja-build clang lld` gets the tools; on Windows, `winget install Git.Git Kitware.CMake Ninja-build.Ninja LLVM.LLVM`, with Visual Studio's Build Tools and the "Desktop development with C++" workload, from [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/), for the C runtime and SDK that LLVM's clang links with. A build from source uses the clang on PATH, or `CALX_CC`; set that to a release's `toolchain\bin\clang.exe` to build programs the way a release does.

That makes `build/compiler/calx`, or `calx.exe` on Windows. It finds its runtime in the checkout it was built from, so leave the checkout where it is. If you move it, set `CALX_HOME` to the new place. `calx version` says `0.1.0-dev (built from source)`, where a release names its commit and date. `calx update` doesn't update a build from source: `git pull` and `ninja` do.

`build/compiler/calx run tests/run.calx` runs the test suite with that compiler, and it includes every example in these docs. CI runs it in debug and release builds, on Windows and Linux, on x64 and on Arm64.

## Next

- Set up your editor: the VS Code extension highlights Calx and shows errors as you type ([editor support](../reference/editors.md)).
- Start the guide with [Getting started](getting-started.md), which writes a first program.
