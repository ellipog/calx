---
title: Modules
description: Files as modules, where the compiler finds them, imports, pub, where impls go, and how each module becomes its own C file.
---

# Modules

A file is a module. A program is its main file and every module that file imports, directly or not.

```calx
// file: main.calx
import shapes.{Square, area_of}

fn main() {
    let s = Square { side: 3.0 }
    println("{area_of(s)} {s.side}")    // out: 9.0 3.0
}

// file: shapes.calx
pub struct Square {
    pub side: f64
}

pub fn area_of(s: Square) -> f64 {
    return s.side * s.side
}
```

The examples on this page are several files each. A line `// file: NAME` starts each one, and the first is the one given to `calx run`.

## Where modules are

Module `a.b.c` is the file `a/b/c.calx` under the source root. The source root is the main file's folder, unless the main file starts with a `module` line, as in `module game.main`. Then the root is the folder that holds `game/`.

```calx
// file: game/main.calx
module game.main

import game.world.{spawn}

fn main() {
    println("{spawn(3)}")    // out: 6
}

// file: game/world.calx
module game.world

pub fn spawn(n: i32) -> i32 {
    return n * 2
}
```

- A file in a folder starts with its `module` line, and the line must match the file's place. A file at the root can leave the line out, and its name is the module's name. This way any file can be checked on its own, as editors do, and still find the root.
- Module and folder names match exactly, case included, on Windows as on Linux.
- Imports can't form a cycle. The error names the modules in the cycle.

## The standard library

Module `std.x` is `std/x.calx` under the Calx home (`CALX_HOME`, [the calx command](command.md#environment)), wherever the program is. It's imported like any module: `import std.io.{read, write}`. `std` is the standard library's name, so a program's own modules can't use it. Its modules have a page each ([std.io](../std/io.md)).

They're compiled like the program's own modules, each to its own C file, which the object cache keeps, so using one costs a build almost nothing after the first.

## Importing

`import a.b.{X, Y}` brings the `pub` items `X` and `Y` of module `a.b` into this one, to use as if they were declared here.

- Each imported name must be used, and it can't be a name this module declares or has already imported, since there's no shadowing.
- `import a.b` alone is an error: say what to import. Reaching items through a module's name, as in `b.f()`, isn't built yet.
- A type's methods and its interfaces' functions come with the type. Once a module has a `Square`, its `pub` methods work, and so do the functions of every `pub` interface it implements, without importing anything more.

## `pub`

What other modules can see:

| Declared | Seen from other modules when |
| --- | --- |
| A function, struct, enum, interface or constant | It's `pub` |
| A struct's field | The field is `pub` |
| A function in an `impl` block | It's `pub` |
| An enum's variants | Always, with the enum |
| An interface's functions and constants | Always, with the interface |

A private field can be read and set only in its module. A literal in another module can leave it out only if it has a default, so a struct with a private field and no default can be built only in its own module, by the functions it provides:

```calx
// file: main.calx
import bank.{Account}

fn main() {
    var a = Account.open()
    a.deposit(5)
    println("{a.balance()}")    // out: 5
}

// file: bank.calx
pub struct Account {
    cents: i64
}

impl Account {
    pub fn open() -> Account {
        return Account { cents: 0 }
    }
    pub fn deposit(a: mut Account, n: i64) {
        a.cents += n
    }
    pub fn balance(self) -> i64 {
        return self.cents
    }
}
```

```calx
// file: main.calx
import bank.{Account}

fn main() {
    let a = Account.open()
    println("{a.cents}")    // error: field `cents` of `Account` is private to `bank`
}

// file: bank.calx
pub struct Account {
    cents: i64
}

impl Account {
    pub fn open() -> Account {
        return Account { cents: 0 }
    }
}
```

A `pub` function's parameters and result, a `pub` struct's `pub` fields and a `pub` enum's variants can't have a type that isn't `pub`, since other modules couldn't name it. `pub` items are never reported as unused (D74).

## Where impls go

- Methods for a type go in the type's module: `impl Square` belongs with `struct Square`.
- An impl of an interface goes in the interface's module or the type's. An impl for a primitive, such as `impl Shape for i32`, goes in the interface's.

So there's one place to look for each impl, and two modules can't implement one interface for one type in different ways.

## In C (D75)

Each module becomes one C file, named after it, such as `game_world.c`. It's compiled to its own object, and the objects are linked into the program. A module's file holds:

- the module's functions;
- a `static` copy of each generic instance those functions use, so every file compiles by itself, in parallel with the others;
- the type definitions, constants and declarations those name, and nothing else.

Since a file holds exactly what it uses, its text changes only when something it uses changes, and the [object cache](command.md#fast-builds-d64) compiles only those files again. The front end still checks the whole program every time.

A function is `static` unless another module's file calls it, which lets clang inline it and drop it. An item's C name starts with its module's: `Square` in `geo.shapes` is `geo_shapes__Square`. `calx emit-c` prints each file after a line `// ---- NAME.c`, and `-o` with a folder writes them there.

Packages (`calx.toml`), reaching items through a module's name, and renaming imports are designed but not built yet ([spec](../CALX_SPEC.md#modules-and-builds)).
