<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/logo/calx-banner-dark.png">
  <img src="assets/logo/calx-banner.png" alt="Calx" width="100%">
</picture>

Calx is a general-purpose compiled language. Its compiler is written in C++ and emits C, which clang builds on Windows and Linux, on x64 and Arm64, and on macOS on Apple Silicon. The largest Calx program so far is [Calcite](https://github.com/ellipog/calcite), a Minecraft Java Edition server in development, which aims to behave exactly as the official server does.

Website and docs: **[calxlang.com](https://calxlang.com)**

What sets it apart:

- **Faster than Rust.** On the tick-loop benchmark, which uses the same algorithms and gives the same checksum as its Rust reference, Calx takes about 0.7× Rust's time per tick, with one thread or 24, on Windows and Linux.
- **Deterministic math.** Integers and floats give the same bits on every platform. The rules match Java's, so code ported from Java gives the same results.
- **Memory-safe by default.** Ownership is checked at compile time and costs nothing at run time: no garbage collector, no reference counts. Unchecked code is opt-in, inside `unsafe`.
- **Deterministic parallelism.** `parallel for` runs a loop's iterations across every core, and the compiler checks that no iteration touches what another one changes. A program prints the same output with one thread or many. Region actors come next.
- **Strict by default.** No implicit conversions, no dropped values, no unused names and no shadowing. Each mistake gets an error that says what to write instead.
- **Fast to build.** `calx run` on a small program takes about 0.3 s from an empty cache and under 25 ms when nothing changed.

Status: Calcite's Phases 0 to 5 have passed their gates, the compiler's core, the protocol, and chunks, storage, regions and world generation among them, and Phase 6, simulation, is under way. The compiler builds integers, floats, arrays, structs, enums, tuples, optionals, `match`, generics, interfaces, modules, `String`, `List`, `Map` and `Set`, error values (`T ! E`, `fail`, `try`), closures, `unsafe` code with C functions, `parallel for`, libraries a program opens while it runs, for plugins, and attributes that libraries define in Calx, which generate code while the program compiles. They give Java's results on every platform. The standard library has:

- **Files, processes and the terminal:** `std.fs`, `std.path`, `std.process`, `std.env`, `std.io`, `std.term`, `std.cli`, `std.shell` and `std.log`.
- **Text and data:** `std.json`, `std.yaml`, `std.toml`, `std.ini`, `std.csv`, `std.xml`, `std.html`, `std.sqlite`, `std.regex`, `std.textwrap`, `std.diff`, `std.unicode`, `std.url` and `std.uuid`.
- **Compression and archives:** `std.zlib`, `std.zstd`, `std.xz`, `std.lz4`, `std.zip` and `std.tar`.
- **Numbers:** `std.math`, `std.bigint`, `std.decimal`, `std.stats`, `std.random` and `std.java.math`.
- **Networking and threads:** `std.net` (TCP driven by events on IOCP and epoll), `std.http` and `std.http.server`, `std.mail` (messages between actors), `std.thread` and `std.pool`.
- **The rest:** `std.crypto`, `std.hash`, `std.image` (PNG and JPEG), `std.time`, `std.meta` (declarations for attributes), `std.build`, and, for Calcite, `std.palette`, `std.text.mutf8` and the Java-compatible collections and strings.

The [reference](docs/README.md#reference) lists what's built.

## Performance

On the [language benchmarks](docs/language-benchmarks.md), Calx against C, Rust, Go and Python on 25 programs, from the Benchmarks Game to JSON, CSV, compression and parallel loops, each written in each language with the same algorithm and measured on an i9-13900K: how many programs Calx took less time on, how many came within 5% either way, and how many took it longer. The parallel loops count on every processor.

| Calx against | Windows 11 | Linux (WSL2) |
| --- | --- | --- |
| C | 13 faster, 5 even, 1 slower | 8 faster, 7 even, 4 slower |
| Rust | 15 faster, 5 even, 5 slower | 13 faster, 7 even, 5 slower |
| Go | 17 faster, 3 even, 5 slower | 17 faster, 4 even, 4 slower |
| Python | 21 faster | 21 faster |

The page has every median, the memory each program held, why Calx is slower where it is, and how long each language takes to build a program and to start one. [calxlang.com/performance](https://calxlang.com/performance/) draws them, for either system.

On the [scripting benchmarks](docs/scripting-benchmarks.md), against Rust and Python on the same machine, each figure is the best times of a benchmark's steps added up, from 5 runs. The page has each step, and the CPU time and memory each program used:

| | Platform | Calx | Rust | Python |
| --- | --- | --- | --- | --- |
| Taking 10.4 MB of CSV apart | Windows 11 | **6.50 ms** | 17.1 ms | 112 ms |
| | Linux (WSL2) | **7.18 ms** | 17.8 ms | 80.0 ms |
| Writing 1.6 million numbers as text | Windows 11 | **18.7 ms** | 45.2 ms | 214 ms |
| | Linux (WSL2) | **20.2 ms** | 45.3 ms | 169 ms |
| 12.6 MB of JSON parsed, written and read into records | Windows 11 | **63.7 ms** | 124 ms | 434 ms |
| | Linux (WSL2) | **65.1 ms** | 72.8 ms | 268 ms |
| A tree of 10,000 files walked and globbed | Windows 11 | **30.6 ms** | 33.0 ms | 72.3 ms |
| | Linux (WSL2) | **3.59 ms** | 5.35 ms | 13.2 ms |
| The smallest program, run | Windows 11 | **7.27 ms** | 7.74 ms | 22.5 ms |
| | Linux (WSL2) | **0.31 ms** | 0.32 ms | 6.27 ms |

Build times on the same machine:

| | Windows 11 | Linux (WSL2) |
| --- | --- | --- |
| `calx check`, 64 modules (9,900 lines) | 32 ms | 24 ms |
| Debug build of those 64 modules, empty cache | 520 ms | 278 ms |
| Debug rebuild after changing one module | 123 ms | 78 ms |
| `calx run` of a small program, empty cache | 274 ms | 302 ms |
| `calx run` of it again, nothing changed | 16 ms | 21 ms |

Milliseconds per tick on the [tick-loop benchmark](docs/benchmarks.md), a fixed simulation written in Calx and in Rust with the same algorithms, measured on an i9-13900K. Each figure is the median of 10 runs of 1,000 ticks. The Rust reference was built with rustc 1.91.1, and Calx with clang 23.

| Platform | Calx, 1 worker | Rust, 1 worker | Calx, 24 workers | Rust, 24 workers |
| --- | --- | --- | --- | --- |
| Windows 11 | **18.06** | 25.75 | **2.25** | 3.41 |
| Linux (WSL2) | **18.01** | 26.04 | **2.46** | 3.52 |

CI runs the tick loop and checks its checksum on every push. It also times the builds. The raw results are in [bench/results](bench/results).

## Installing

On Windows, in PowerShell:

```powershell
irm https://calxlang.com/install.ps1 | iex
```

On Linux:

```sh
curl -fsSL https://calxlang.com/install.sh | sh
```

Each brings the toolchain it builds programs with, clang and lld from LLVM 23, so there's nothing else to install. Then `calx update` keeps it up to date ([Install Calx](docs/guide/install.md)).

## Building

To build the compiler from source, you need CMake, Ninja and clang (LLVM 18 or later, with lld). [Install Calx](docs/guide/install.md#building-from-source) has the steps for each platform.

```sh
cmake -S compiler -B build/compiler -G Ninja -DCMAKE_CXX_COMPILER=clang++
ninja -C build/compiler
build/compiler/calx run tests/basics/hello.calx
```

`build/compiler/calx run tests/run.calx` runs the test suite with that compiler, and it includes every example in the docs. CI runs it in debug and release on Windows and Linux, on x64 and Arm64, and on macOS.

## Editors

The VS Code extension in [tools/vscode](tools/vscode/README.md) highlights Calx, shows the compiler's errors as you type, and runs files. Install it with `calx run tools/vscode/package.calx -- --install`. Other editors can use its TextMate grammar and `calx check --json` ([editor support](docs/reference/editors.md)).

Acta board: aaen-studios/clx ([open](https://acta.aaenz.no/aaen-studios/clx)). The phase gates are on Calcite's board, aaen-studios/calcite.

## Docs

Read them on [calxlang.com/docs](https://calxlang.com/docs/), or here, starting at [docs/README.md](docs/README.md). To learn the language, follow the [guide](docs/guide/getting-started.md), which builds a small program step by step. The docs are written alongside the language, so they always describe what's actually built, and the site is rebuilt from them on every push to main.

## License

MIT
