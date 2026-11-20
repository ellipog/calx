---
title: Packages
description: Using Calx code from another folder or another git repository: calx.toml, path and git dependencies, calx.lock, and working offline.
---

# Packages

A program's own modules are found next to its main file ([modules](../reference/modules.md)). Code from somewhere else, a library in the folder beside it or in another git repository, comes in as a **package**: a folder with a `calx.toml` that gives its name, its version and the packages it depends on. It's Cargo's `Cargo.toml` with `path =` and `git =` dependencies, or pip's `pip install -e ../lib` and `git+https://...`, written down once so every build uses the same code.

There's no registry yet, so a package comes from a path or a git URL. Quarry, the registry, builds on this later.

## A package

`app` uses the package `shapes`, kept in `libs/shapes`:

```calx
// file: main.calx
import shapes.{Square}
import shapes.circle

fn main() {
    let s = Square { side: 3.0 }
    println("{s.area()} {circle.area(1.0)}")    // out: 9.0 3.0
}

// file: calx.toml
[package]
name = "app"
version = "0.1.0"

[dependencies]
shapes = { path = "libs/shapes" }

// file: libs/shapes/calx.toml
[package]
name = "shapes"
version = "1.0.0"

// file: libs/shapes/shapes.calx
import circle.{area}

pub struct Square {
    pub side: f64
}

impl Square {
    pub fn area(self) -> f64 {
        return self.side * self.side
    }
}

pub fn unit_circle() -> f64 {
    return area(1.0)
}

// file: libs/shapes/circle.calx
pub fn area(r: f64) -> f64 {
    return 3.0 * r * r
}
```

- A program imports a package's modules under its name. Module `circle` of `shapes`, the file `circle.calx` in the folder its calx.toml is in, is `shapes.circle`, and `a/b.calx` there would be `shapes.a.b`. The package's module of its own name, `shapes.calx`, is `shapes`, as a crate's `lib.rs` is the crate.
- Inside the package, its modules use their own names: `shapes.calx` imports `circle`, not `shapes.circle`. So a package checks, builds and tests by itself, as any program does.
- `calx run main.calx`, `check`, `build` and `test` find the calx.toml nearest the main file, in its folder or a folder above it, and the packages it lists. An editor checking any file of the program finds the same one.
- A program with no calx.toml above it builds as it always has.

## calx.toml

```toml
[package]
name = "hird"
version = "0.3.0"

[dependencies]
util = { path = "../util" }
spar = { git = "https://git.example.com/spar.git", tag = "v0.2.0" }
loom = { git = "https://git.example.com/loom.git", branch = "main" }
pinned = { git = "https://git.example.com/pinned.git", rev = "3f9c2a1b" }
```

| Key | Means |
| --- | --- |
| `name` | How programs import the package: lowercase letters, digits and `_`, starting with a letter, and not a keyword or `std` |
| `version` | Three numbers, as `"0.3.0"`, with `-pre` after them if wanted, as `"1.0.0-beta.1"` |
| `path` | A package in a folder, from the folder this calx.toml is in |
| `git` | A package in a git repository, at the repository's root. With it, one of: |
| `tag` | a tag |
| `branch` | a branch, pinned to the commit it was on when it was first fetched |
| `rev` | a commit, by its hash or at least its first 4 digits |

A `git` dependency with none of the three uses the repository's default branch. Each dependency can be a table of its own too, as `[dependencies.util]` with `path = "../util"` under it. The name a calx.toml lists a dependency by is that package's own name.

calx reads TOML strictly: a key it doesn't know is an error that says which ones it has, so a typo like `brnach` doesn't quietly do nothing.

## Git dependencies and calx.lock

The first build that needs a git dependency fetches it, with `git` from PATH, into the [cache](../reference/command.md#packages), and writes `calx.lock` beside calx.toml with the commit it was pinned to:

```toml
version = 1

[[git]]
name = "spar"
url = "https://git.example.com/spar.git"
tag = "v0.2.0"
commit = "0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c"
```

Keep calx.lock in version control with calx.toml, as `Cargo.lock` is kept. Every build after that, on any machine, uses the same commits, and one that finds a pinned commit in the cache already runs no git at all, so it works offline. A branch that moves on doesn't move a build pinned to it. To move a dependency, change its tag, branch or rev, or take its entry out of calx.lock, and the next build fetches and pins it again.

Only the calx.lock of the package whose program is built counts. A dependency's own calx.lock is left alone, as Cargo does.

## When something's wrong

Each mistake is an error at its place in calx.toml, saying what to write:

```text
calx.toml:6:17: error: `../nope`, the path given for `nope`, isn't a folder: it's `/home/me/nope`
calx.toml:6:46: error: https://git.example.com/spar.git has no tag `v9`; its tags are v0.1.0, v0.2.0
```

- **Two packages of one name**, such as `util` from `../util` for one package and from git for another, are an error that names both: a program has one package of each name, so both have to point at the same one. Two packages that depend on the same folder, or the same commit, share it.
- **Packages can't depend on each other in a cycle**, as modules can't import each other in one.
- **A package the program has but doesn't list**, because only a dependency of it does, can't be imported until calx.toml lists it too, so what a package uses is written in its calx.toml.
- **A module of the program's own named as a package** is an error: `util.calx` beside a calx.toml that lists `util` would make `import util` mean two things.
