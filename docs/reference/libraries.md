---
title: Libraries
description: Building a library with `calx build --lib` and `@export`, opening it from a program with `Library`, checking the types of what it exports, and the one runtime the two share.
---

# Libraries

A library is Calx code that a program opens while it runs, such as a server's plugin. `calx build --lib` builds one, a `.dll` on Windows and a `.so` on Linux, which exports the functions marked `@export`. The program opens it with `Library.open`, finds each function by its name and its type, and calls it as a [function value](functions.md#functions-as-values) (D123).

```calx
// lib: greeter.calx
// file: main.calx
import std.env.{args}

fn main() ! String {
    let given = args()
    let lib = unsafe { try Library.open(given[0]) }
    let greet = try lib.find<fn(str) -> String>("greet")
    println(greet("plugin"))  // out: hello, plugin
}

// file: greeter.calx
@export
fn greet(name: str) -> String {
    return "hello, {name}"
}
```

The test suite builds `greeter.calx` as a library and passes its path as the program's first argument. A real program names its libraries itself, as a server lists the files in its plugin folder.

## Building a library

```sh
calx build --lib plugin.calx                 # plugin.dll, or plugin.so
calx build --lib --release plugin.calx -o plugins/plugin.dll
calx check --lib plugin.calx
```

A library is a main file and the modules it imports, as a program is, without a `main`. It exports at least one function. `@export` goes on a function of its own, not one in an `impl` or an interface, and exports it under its name:

```calx
// kind: library
struct Counter {
    n: i32
}

impl Counter {
    @export  // error: `@export` is for a function on its own, not one in an `impl`
    fn get(self) -> i32 {
        return self.n
    }
}

@export
fn first<T>(xs: List<T>) -> T {  // error: an `@export` function can't be generic
    return xs[0]
}

@export
fn cx_attach() {}  // error: names that start with `cx_` are the runtime's

@export
fn count(c: Counter) -> i32 {
    return c.n
}
```

An exported function can't be generic, since a program finds it by a name, which has one type. It can't be `@track_caller`, since the program calls it through a function value, which can't say where it was called from. Two modules of one library can't export the same name, and names that start with `cx_` are the runtime's.

Otherwise an exported function takes and gives what any function does: numbers, text, lists, maps, structs and enums, optionals and results, and other function values, with any [parameter modes](functions.md). A struct the program and the library both use comes from a module they both import.

In a program, `@export` changes nothing.

## Opening a library

| Call | What it does |
| --- | --- |
| `unsafe { Library.open(path) }` | Opens the library at `path`, a `str` or a `String`, from the current folder. Gives `Library ! String`: the reason, when it fails, is that there's no such file, that it isn't a Calx library, or that another build of Calx built it |
| `lib.find<F>(name)` | The function the library exports as `name`, as an `F`, which is a `fn(...)` type. Gives `F ! String`: the reason, when it fails, is that the library exports nothing by that name, or that what it exports has another type |
| `lib.has(name)` | Whether the library exports a function named `name` |
| `unsafe { lib.close() }` | Unloads the library, and uses up `lib` |

`find` can take its type from where its value goes, as in `let f: fn(i32) -> i32 = lib.find("double")!`.

Opening a library needs `unsafe`, since it runs code from outside the program, which the checker never saw: as with a [C function](unsafe.md), the program vouches for it. Finding a function is safe, since what it finds has the type asked for, and the library stays loaded while anything could hold a function from it. Dropping a `Library` leaves it loaded. `close` unloads it, which needs `unsafe`, since a function found in it would then call nothing. A library opened twice is unloaded at the second `close`.

A result goes through an `unsafe` block that holds only its call, so `match unsafe { Library.open(path) } { ... }`, `try unsafe { Library.open(path) }` and `unsafe { Library.open(path) }!` handle it where the call is ([errors](errors.md)).

A `Library` can go to another thread. It can't be copied, since it's closed once, by whatever holds it, and it isn't compared, shown, hashed or ordered:

```calx
fn main() {
    let lib = unsafe { Library.open("plugin.dll")! }
    _ = lib.copy()  // error: `Library` can't be copied; a library is closed once, by whatever holds it
    _ = lib.find("double")  // error: can't tell what type of function to find
    _ = lib.find<i32>("double")  // error: `find` gives a function, so what it finds is a `fn(...)`, not an `i32`
    lib.close()  // error: `close` unloads the library, after which a function found in it would call nothing
}
```

### Types match all the way down

`find` checks more than the type's name. A `Point` in the program and a `Point` in the library are one type only when each has the same fields, of the same types, in both builds, and so on for every struct and enum in the function's type. A library built with another version of a module the two share is refused, rather than reading a struct with the wrong layout:

```calx
// lib: plugin/plugin.calx
// file: main.calx
import std.env.{args}
import shapes.{Point}

fn main() {
    let given = args()
    let path = given[0].copy()
    let lib = unsafe { Library.open(path)! }
    match lib.find<fn() -> Point>("origin") {
        ok(_) => println("found it")
        err(e) => println("PATH{e[path.len()..]}")
    }
    // out: PATH's `origin` is `fn() -> Point` here too, but a type in it isn't the same in the two builds; build them from the same source
}

// file: shapes.calx
pub struct Point {
    pub x: i32
    pub y: i32
}

// file: plugin/plugin.calx
import shapes.{Point}

@export
fn origin() -> Point {
    return Point { x: 0, y: 0, z: 0 }
}

// file: plugin/shapes.calx
pub struct Point {
    pub x: i32
    pub y: i32
    pub z: i32
}
```

## One runtime

A library has no runtime of its own. The program that opens it gives it its own: one allocator, one output, one way to panic, and the threads `parallel for` runs on. So memory the library allocates, such as a `String` it gives back, the program keeps and frees, and a debug build's check for memory that was never freed covers both. A panic in a library is the program's, with a stack trace through both:

```calx
// lib: plugin.calx
// file: main.calx
import std.env.{args}

fn main() {
    let given = args()
    let lib = unsafe { Library.open(given[0])! }
    let pick = lib.find<fn(List<i32>, usize) -> i32>("pick")!
    var xs = List<i32>()
    xs.push(7)
    println("{pick(xs, 3)}")
}

// file: plugin.calx
@export
fn pick(xs: List<i32>, i: usize) -> i32 {
    return xs[i]  // panic: index 3 is out of bounds for length 1
    // trace: plugin.pick:165
    // trace: main.main:159
}
```

The program and its libraries must be built by the same build of `calx`, since the compiler decides how values are laid out and passed, and the runtime's functions are listed in the order its header has them. `Library.open` refuses a library another build made. A debug build and a release build can open each other's.

## In C

A library's C is compiled with `-DCX_LIBRARY`, which makes each runtime function [the header](../../runtime/calx_rt.h) declares with `CX_FN` a pointer rather than a function:

```c
CX_FN(void*, cx_alloc, (cx_usize size));
/* a program:  void* cx_alloc(cx_usize size);      */
/* a library:  extern void* (*cx_alloc)(cx_usize size); */
```

The library exports one symbol, `cx_library`, which holds a hash of the compiler and the runtime header it was built with, how many runtime functions it takes, the function that fills in its pointers, and the function that lists its exports: each one's name, address, type as Calx writes it, and a hash of its shape, which is its type with the fields and variants of every struct and enum in it. `Library.open` checks the hash and fills in the pointers from the program's own, in the header's order. `find` compares the shape.

So a call from a library into the runtime is one indirect call, as a call into a DLL is on Windows, and a program's own calls are direct: opening libraries costs the program nothing. A program that opens libraries links the whole runtime, so any library finds every function it needs. On Linux a library is compiled with `-fPIC -fvisibility=hidden`, so `cx_library` is the only symbol it shows.

A path is from the current folder on both systems, and Windows loads the DLLs a library needs from the library's own folder, then the system's. Stack traces read each library's line tables as they read the program's: from the PDB beside it on Windows, which `calx build` puts there, and from the library itself on Linux.
