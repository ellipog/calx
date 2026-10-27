# The Calx compiler

`calx` is written in C++20 and turns a `.calx` file into C, which clang compiles and links against the runtime in [`../runtime`](../runtime). The language it accepts is described in the [reference](../docs/README.md#reference).

## Pipeline

| Stage | Files | Does |
| --- | --- | --- |
| Sources | `source.h`, `source.cpp` | Loads files, maps offsets to lines and columns, and collects and prints diagnostics, as text or as JSON lines for editors |
| Lexer | `lexer.h`, `lexer.cpp` | Turns text into tokens, and records which tokens start a line, since lines end statements |
| Parser | `parser.h`, `parser.cpp`, `ast.h` | Builds the syntax tree for a whole file. It parses every planned feature, so the checker can say which ones aren't built yet |
| Checker | `check.h`, `check.cpp`, `types.h`, `types.cpp` | Resolves names, gives every expression a type, folds constants and applies the strictness rules (D63). It writes what it finds into the tree, such as each call's kind and each name's target |
| Emitter | `emit.h`, `emit.cpp` | Writes one C file from the checked tree, keeping Java's evaluation order and semantics |
| Driver | `toolchain.h`, `toolchain.cpp`, `main.cpp` | Finds clang, caches objects and executables, runs clang in parallel and links (D64) |

Everything the tree points at lives in one arena per compile (`common.h`), and names are interned, so comparing two names compares two integers.

## Rules for the code

- **Errors don't cascade.** After an error, mark whatever it touched as used and give it the error type, so each mistake gets one message. `is_bad(t)` checks for the error type before reporting anything about it.
- **Messages say what to write.** An error names the fix when there's one, such as "declare it with `let`". Messages use backticks for code and say "an `i64`", not "a `i64`" (`a_type`).
- **Java's semantics live in the emitter and the runtime**, never in clang's defaults. Integer arithmetic goes through unsigned types, division through helpers that check for zero and `MIN / -1`, and float conversions through range checks.
- **Every feature comes with tests and a reference page** (D40, D41). The examples in the reference are tests too.

## Building

```sh
cmake -S compiler -B build/compiler -G Ninja -DCMAKE_CXX_COMPILER=clang++
ninja -C build/compiler
```

`python tests/run.py` builds the compiler into `build/compiler/<platform>` and runs the tests. `--modes debug,release` runs each test in both build modes, `--no-build` skips building, and a path runs one test.

## Tests

Tests are `.calx` files under [`../tests`](../tests), plus every `calx` code block in the reference docs. Comments on a line say what that line must do:

| Comment | Means |
| --- | --- |
| `// out: text` | The program prints this line. The lines are matched in order |
| `// error: message` | Compiling fails with an error on this line whose message contains `message`. The file must produce exactly these errors |
| `// panic: message` | The program panics on this line, with exit status 101 and this message |

## Debugging the output

`calx emit-c file.calx` prints the C. It has `#line` directives, so a debugger steps through the Calx source. `--no-line` leaves them out, for reading the C itself.
