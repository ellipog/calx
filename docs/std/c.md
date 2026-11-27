---
title: std.c
description: Opening a C library while the program runs and finding its functions, as Python's ctypes.CDLL does, in programs and static libraries, on Windows, Linux, macOS and iOS.
---

# `std.c`

A C library opened while the program runs, a `.dll` on Windows, a `.so` on Linux and a `.dylib` on macOS, and the addresses of the functions it exports, which [`p.to_fn<F>()`](../reference/unsafe.md#c-function-values) turns into functions Calx calls, as Python's `ctypes.CDLL(path)` and `lib.name` do (CLX-477). It's for a library the program finds only when it runs, such as one it downloaded, or one that's on some systems and not others. A library known when the program builds is linked instead, with an [`extern "C"` block](../reference/unsafe.md#c-functions). A Calx library, which `calx build --lib` builds, is opened with [`Library.open`](../reference/libraries.md), which checks the types of what it finds.

```calx
import std.c.{CLibrary}
import std.env.{os}

fn main() {
    let name = match os() {
        "windows" => "ucrtbase.dll",
        "macos" => "libSystem.dylib",
        _ => "libc.so.6",
    }
    let libc = unsafe { CLibrary.open(name)! }
    let abs = unsafe { libc.symbol("abs").to_fn<extern "C" fn(i32) -> i32>() }
    println("{unsafe { abs(-6) }}")    // out: 6
    println("{libc.has("abs")} {libc.has("no_such_function")}")    // out: true false
    match unsafe { CLibrary.open("no-such-library.dll") } {
        ok(_) => println("opened it"),
        err(e) => println("{e.starts_with("can't open no-such-library.dll: ")}"),    // out: true
    }
    unsafe { libc.close() }
}
```

| Item | Does |
| --- | --- |
| `unsafe { CLibrary.open(path) } -> CLibrary ! String` | Opens the C library at `path`. It fails with `can't open PATH: ` and the system's reason, such as that there's no such file, or that a library it needs isn't there |
| `unsafe { lib.symbol(name) } -> *u8` | The address of what the library exports as `name`, a function or data, or null if it exports nothing by that name |
| `lib.has(name) -> bool` | Whether the library exports `name` |
| `unsafe { lib.close() }` | Unloads the library, and uses up `lib` |

Opening a library needs `unsafe`, since it runs the library's code, which the checker never saw. So does `symbol`, which gives a pointer, and calling what `to_fn` makes of it, which is calling C: the program vouches that the function has the type it says. `close` needs `unsafe`, since a function found in the library would then call nothing. Dropping a `CLibrary` leaves the library loaded. A library opened twice is unloaded at the second `close`.

A `CLibrary` is a [handle](../reference/ownership.md#handles): it can't be copied, since it's closed once, by whatever holds it. It can go to another thread.

```calx
import std.c.{CLibrary}

fn main() {
    let lib = CLibrary.open("libfoo.so")!    // error: `open` is an `unsafe` function, so calling it needs `unsafe`
    _ = lib.copy()    // error: `CLibrary` can't be copied
    lib.close()    // error: `close` is an `unsafe` function, so calling it needs `unsafe`
}
```

## Where it looks

- **A path**, with a folder in it, such as `models/sherpa-onnx-c-api.dll` or `/opt/lib/libfoo.so`, is from the current folder, as a file's is.
- **A name** without a folder, such as `kernel32.dll` or `libc.so.6`, is found where the system finds libraries: the program's folder and the system's on Windows, and the system's library path elsewhere, as `ctypes` finds one. A file in the current folder is `./name`.
- **What the library needs**, the libraries it's linked against, are loaded from its own folder on Windows, then the system's, so a DLL shipped beside it is found, as onnxruntime.dll is beside sherpa-onnx-c-api.dll. Never from the current folder or `PATH`. On Linux and macOS they're found as the library was linked to find them, such as by an rpath of `$ORIGIN` or `@loader_path`.
- **Its symbols** are all resolved as it opens, so a library missing one fails then, not when it's called. On Linux and macOS they're kept to itself, so a library opened later doesn't use them in place of its own.

## Static libraries and iOS

A [static library](../reference/embedding.md) opens C libraries as a program does, on the host's thread or any other. On Linux, its host links `-ldl`, as the header made for it says.

iOS runs only code signed into the app, so an app can open a library it ships with, such as a framework in its bundle, and never one it downloaded. Opening one fails, and the error says so: `can't open PATH: ...; iOS opens only libraries signed into the app's bundle, so never one it downloaded`. A library an iOS app needs is linked into it instead.

## In C

`CLibrary` holds the system's handle: an `HMODULE` from `LoadLibraryExW` on Windows, with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` for a path, and `dlopen(path, RTLD_NOW | RTLD_LOCAL)` elsewhere. `symbol` is `GetProcAddress` or `dlsym`, and `close` is `FreeLibrary` or `dlclose`. They're in [runtime/calx_clib.c](../../runtime/calx_clib.c), which `Library.open` loads Calx libraries with too, and which is linked only into programs and static libraries that open one.
