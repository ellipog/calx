<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/logo/calx-logo-white.svg">
  <img src="assets/logo/calx-logo.svg" alt="Calx" height="72">
</picture>

Calx is a compiled systems language built for [Calcite](https://github.com/ellipog/calcite), a Minecraft Java Edition server. Its compiler is written in C++ and emits C, which clang builds on Windows and Linux.

What sets it apart:

- **Java-exact math.** Integers and floats behave exactly as they do in Java, so Calcite can reproduce vanilla Minecraft bit for bit.
- **Memory-safe by default.** Unchecked code is opt-in and limited to the server core and plugins you trust.
- **Region actors.** The only concurrency model. Results are deterministic whatever the thread count.

Status: Phase 0, design on paper. There's no compiler yet.

Acta board: aaen-studios/clx ([open](https://acta.aaenz.no/aaen-studios/clx)). The phase gates are on Calcite's board, aaen-studios/calcite.

## Docs

Start at [docs/README.md](docs/README.md). The docs are written alongside the language, so they always describe what's actually built.

## License

MIT
