<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/logo/calx-logo-white.svg">
  <img src="assets/logo/calx-logo.svg" alt="Calx" height="72">
</picture>

Calx is a compiled systems language built for [Calcite](https://github.com/ellipog/calcite), a Minecraft Java Edition server. Its compiler is written in C++ and emits C, which clang builds on Windows and Linux.

What sets it apart:

- **Java-exact math.** Integers and floats behave exactly as they do in Java, so Calcite can reproduce vanilla Minecraft bit for bit.
- **Memory-safe by default.** Unchecked code is opt-in and limited to the server core and plugins you trust.
- **Region actors.** The only concurrency model. Results are deterministic whatever the thread count.

Status: Phase 3. The compiler builds and runs programs with integers, floats, arrays, structs, methods and the control flow, with Java's results on Windows and Linux. Enums, optionals, generics, modules, the standard library and actors come next. The [reference](docs/README.md#reference) lists what's built.

## Building

You need CMake, Ninja and clang (LLVM 18 or later, with lld).

```sh
cmake -S compiler -B build/compiler -G Ninja -DCMAKE_CXX_COMPILER=clang++
ninja -C build/compiler
build/compiler/calx run tests/basics/hello.calx
```

`python tests/run.py` builds the compiler and runs the test suite, which includes every example in the reference.

Acta board: aaen-studios/clx ([open](https://acta.aaenz.no/aaen-studios/clx)). The phase gates are on Calcite's board, aaen-studios/calcite.

## Docs

Start at [docs/README.md](docs/README.md). The docs are written alongside the language, so they always describe what's actually built.

## License

MIT
