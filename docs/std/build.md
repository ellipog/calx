---
title: std.build
description: The program's build ID, a hash of its code that changes exactly when the code does, for keying caches.
---

# `std.build`

The program's own build. A program that keeps a cache of work it did, such as data it parsed, can key the cache on its build ID. A new build of the same code finds the cache still good, and a build with any change to the code makes a new one, so a cache written by older code is never read by newer code.

```calx
import std.build.{build_id}

fn main() {
    let id = build_id()
    println("{id.len()}")    // out: 32
    var hex = true
    for b in id.to_bytes() {
        if not ((b >= 48 and b <= 57) or (b >= 97 and b <= 102)) {
            hex = false
        }
    }
    println("{hex} {id == build_id()}")    // out: true true
}
```

| Function | Gives |
| --- | --- |
| `build_id() -> String` | 32 lowercase hex digits: a hash of the program's generated C and the runtime it links |

- **The same code gives the same ID**, built by the same compiler, in debug and in release, on any machine. The build's flags aren't in it.
- **Any change gives a new ID**: to the program's own modules, to a std module it uses, to the runtime, or to the compiler, where that changes the C it writes.
- **It costs nothing to run.** The ID is a constant in a small file of its own, which the compiler writes and compiles only for programs that call `build_id`. When the code changes, that file is the one extra thing compiled.
- It isn't a version number: two IDs only say whether the code was the same.

`std.build` is written in Calx: [std/build.calx](../../std/build.calx). The ID comes from the compiler's [build](../reference/command.md).
