---
title: Static libraries
description: Building Calx into a C, C++, Rust or Swift program with calx build --staticlib - the functions it exports with @export_c, the header made for them, calx_init and calx_shutdown, linking on each platform, and threads.
---

# Static libraries

`calx build --staticlib` builds Calx code into a static library, a `.lib` on Windows and a `.a` elsewhere, which a program in another language links into itself, as it would one written in C (CLX-334). The library carries the Calx runtime with it, and exports the functions marked [`@export_c`](unsafe.md#calling-calx-from-c), with C types. A header for them is made beside it. It's how an app written in Rust, Swift or C++ keeps its core in Calx, the way a Rust crate is built with `crate-type = ["staticlib"]`.

```sh
calx build --staticlib core.calx                 # core.lib and core.h, or libcore.a and core.h
calx build --staticlib --release core.calx -o out/libcore.a
calx check --staticlib core.calx
```

A static library is a main file and the modules it imports, as a program is, without a `main`. It exports at least one function with `@export_c`. Unlike a [library](libraries.md) a Calx program opens, it shares nothing with another Calx program: each has a runtime of its own, and a process should link one.

## The C it exports

A host passes what C can't, such as text, JSON or a list, as a pointer and a length, and gets events back through a function it gives, with a pointer of its own that it gets back each time. This core takes its configuration as JSON, and tells the host what happens:

```calx
// kind: staticlib
import std.json
import std.json.{Json}

/// Where the core tells the host what happened: the host's function, and the context it gave.
struct Sink {
    on_event: extern "C" fn(*u8, *const u8, usize)
    ctx: *u8
}

impl Sink {
    fn tell(self, message: String) {
        unsafe { (self.on_event)(self.ctx, message.as_ptr(), message.len()) }
    }
}

struct Core {
    sink: Sink
    name: String
}

/// The `len` bytes at `p`, as text, or none if they aren't UTF-8.
unsafe fn text_at(p: *const u8, len: usize) -> String? {
    var bytes = List<u8>()
    for i in 0..len {
        bytes.push(unsafe { p.add(i).read() })
    }
    return String.from_utf8(bytes)
}

/// Starts the core with its configuration, JSON in the `len` bytes at `config`, and tells
/// `on_event`, with `ctx`, what happens. Gives back the core, which the other calls take.
@export_c
fn core_start(config: *const u8, len: usize, on_event: extern "C" fn(*u8, *const u8, usize), ctx: *u8) -> *u8 {
    let sink = unsafe { Sink { on_event: on_event, ctx: ctx } }
    var name = String("core")
    if let given = unsafe { text_at(config, len) } {
        if let ok(Json.Object(o)) = json.parse(given) {
            if let Json.Str(s) = o.find("name") {
                name = s.copy()
            }
        }
    }
    sink.tell("started {name}")
    unsafe {
        return Box(Core { sink: sink, name: name }).into_ptr().cast<u8>()
    }
}

/// Stops the core and frees it.
@export_c
fn core_stop(core: *u8) {
    let c = unsafe { core.cast<Core>().to_box() }
    c.value.sink.tell("{c.value.name} stopped")
}
```

Calx has no global variables, so what a library keeps between calls lives in a [box](boxes.md) C holds: `Box(x).into_ptr()` gives it to C, and `p.to_box()` takes it back, to use and free, or to give back again with `into_ptr`. The test suite's [`tests/library/staticlib.calx`](../../tests/library/staticlib.calx) is a fuller one, with a thread of the library's that runs a std.net event loop and calls the host back from there, and a message queue to it.

## The header

`calx build --staticlib core.calx` writes `core.h` beside `core.lib`, named for the library, without a `.a`'s `lib`:

```c
/* core.h: the C functions of the Calx static library core, which `calx build --staticlib` made.
 * ...how to call and link it... */
#ifndef CALX_CORE_H
#define CALX_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the library's runtime. Calling it again while it runs does nothing. */
void calx_init(void);

/* Ends it: what the library printed is written out. A debug build of the library then checks
 * that everything it allocated was freed, and returns 102 if not, having said so; otherwise 0. */
int calx_shutdown(void);

/// Starts the core with its configuration, JSON in the `len` bytes at `config`, and tells
/// `on_event`, with `ctx`, what happens. Gives back the core, which the other calls take.
uint8_t* core_start(const uint8_t* config, size_t len, void (*on_event)(uint8_t*, const uint8_t*, size_t), uint8_t* ctx);

/// Stops the core and frees it.
void core_stop(uint8_t* core);

#ifdef __cplusplus
}
#endif

#endif
```

Each function's `///` comment is copied into it. Calx's types are C's: `i32` is `int32_t`, `usize` is `size_t`, `bool` is `bool`, `*T` is `T*`, `*const T` is `const T*`, and an `extern "C" fn` is a function pointer.

## Starting and stopping

The host calls `calx_init` before anything else in the library, and `calx_shutdown` last:

- `calx_init` does what a program's start does for the whole process, without its command line, which is the host's: std.env's `args` is empty. Calling it again while the runtime runs does nothing.
- `calx_shutdown` writes out what the library printed and puts the terminal back as it was. The library's threads must have stopped first, as a stop function of its own would see to. A debug build then checks that everything Calx allocated was freed, says what wasn't, and gives 102; otherwise it gives 0.
- A call into the library before `calx_init`, or after `calx_shutdown`, is refused: the runtime says which function was called and ends the process with status 101.
- A panic in the library ends the process, as it does in a program, having said where. Rust's `catch_unwind` and C++'s `catch` don't catch it. Code that can fail returns a status, such as a negative number, for the host to check.

Any of the host's threads can call in, and the library's threads can call the host back, at the same time ([callbacks on C's threads](unsafe.md#callbacks-on-cs-threads)).

## Linking

The library holds the runtime's code, not the system's, so the host links the system libraries it uses:

| Platform | The host links |
| --- | --- |
| Windows | Nothing more: the library names the system libraries it needs, such as `ws2_32.lib`, for the linker. It's built for the MSVC ABI with the DLL C runtime, `/MD`, which is what Rust's `x86_64-pc-windows-msvc` and Visual Studio's defaults use |
| Linux | `-lm -lpthread -ldl` |
| macOS | Nothing more |
| iOS | Nothing more: build it as an [XCFramework](#ios-and-swift) for Xcode |

### C and C++

```sh
clang host.c core.lib -o host.exe -fms-runtime-lib=dll -fuse-ld=link -Wl,-nodefaultlib:libcmt   # Windows, with clang
cl /MD host.c core.lib                                                                          # Windows, with Visual Studio's compiler
clang host.c libcore.a -o host -lm -lpthread -ldl                                               # Linux
clang host.c libcore.a -o host                                                                  # macOS
```

On Windows, clang names the static C runtime, `libcmt`, in every link it starts for the MSVC ABI. A host built for the DLL one, as the library is, leaves it out with `-Wl,-nodefaultlib:libcmt`, or the link mixes the two and can't find C functions such as `__imp_strcspn`. A host built with `/MT`, the static C runtime, can't link the library yet.

```c
#include <stdio.h>
#include <string.h>
#include "core.h"

static void on_event(uint8_t* ctx, const uint8_t* text, size_t len) {
    (void)ctx;
    printf("%.*s\n", (int)len, (const char*)text);
}

int main(void) {
    calx_init();
    const char* config = "{\"name\": \"loom\"}";
    uint8_t* core = core_start((const uint8_t*)config, strlen(config), on_event, NULL);
    core_stop(core);
    return calx_shutdown();
}
```

### Rust

Rust declares the functions in an `extern "C"` block, as it would a C library's, and links the library by name. With Cargo, a `build.rs` says where it is:

```rust
// build.rs
fn main() {
    println!("cargo:rustc-link-search=native=out");
    println!("cargo:rustc-link-lib=static=core");
    if cfg!(target_os = "linux") {
        println!("cargo:rustc-link-lib=dylib=m");
    }
}
```

```rust
// src/main.rs
use std::os::raw::c_void;

extern "C" {
    fn calx_init();
    fn calx_shutdown() -> i32;
    fn core_start(config: *const u8, len: usize, on_event: extern "C" fn(*mut u8, *const u8, usize), ctx: *mut u8) -> *mut u8;
    fn core_stop(core: *mut u8);
}

extern "C" fn on_event(_ctx: *mut u8, text: *const u8, len: usize) {
    let bytes = unsafe { std::slice::from_raw_parts(text, len) };
    println!("{}", String::from_utf8_lossy(bytes));
}

fn main() {
    let config = br#"{"name": "loom"}"#;
    unsafe {
        calx_init();
        let core = core_start(config.as_ptr(), config.len(), on_event, std::ptr::null_mut::<c_void>().cast());
        core_stop(core);
        std::process::exit(calx_shutdown());
    }
}
```

Without Cargo, `rustc main.rs -L native=out -l static=core` links it. The `ctx` pointer is how a callback reaches the host's state, such as an `Arc` given with `Arc::into_raw`, and Rust decides whether that state may be used on the library's threads, as it would for a C library's.

### iOS and Swift

On a Mac with Xcode, `--target aarch64-ios` builds the library for an iPhone and `--target aarch64-ios-sim` for the simulator ([targets](command.md#targets)). `--xcframework` builds both and puts them in an XCFramework, which an Xcode project links as one library, whichever it builds for (CLX-408):

```sh
calx build --xcframework core.calx                           # core.xcframework
calx build --xcframework --release core.calx -o ios/Core.xcframework
calx build --staticlib --target aarch64-ios core.calx        # libcore.a and core.h, for an iPhone alone
```

The XCFramework has a folder for each, `ios-arm64` and `ios-arm64-simulator`, with the library and the header. Beside the header is a module map, so Swift imports the library as a module named for it, `import Core` for `Core.xcframework`. Both are in a folder named for the library, under `Headers`, so two XCFrameworks in one app don't put a `module.modulemap` in the same place. It's made with `xcodebuild -create-xcframework`, and replaces the XCFramework at `-o` if there is one; anything else there is an error. `--xcframework` is for `build` and `check`, takes no `--target`, and takes `--release` and `--cpu` as `--staticlib` does.

In Xcode, add it under the target's Frameworks, Libraries, and Embedded Content, as Do Not Embed, since it's a static library, or name it in a Swift package as `.binaryTarget(name: "Core", path: "Core.xcframework")`. The app links nothing more. Swift passes a closure that captures nothing as the callback, and reaches its own state through the context pointer:

```swift
import Core
import Foundation

final class Events {
    func handle(_ text: String) { print(text) }
}

calx_init()
let events = Events()
let ctx = Unmanaged.passRetained(events).toOpaque().assumingMemoryBound(to: UInt8.self)
let config = Array(#"{"name": "loom"}"#.utf8)
let core = core_start(config, config.count, { ctx, text, len in
    let events = Unmanaged<Events>.fromOpaque(UnsafeRawPointer(ctx!)).takeUnretainedValue()
    events.handle(String(decoding: UnsafeBufferPointer(start: text, count: len), as: UTF8.self))
}, ctx)
core_stop(core)
Unmanaged<Events>.fromOpaque(UnsafeRawPointer(ctx)).release()
_ = calx_shutdown()
```

The callback comes on the library's threads, so an app hands each event to its main actor, with `Task { @MainActor in ... }` or `DispatchQueue.main.async`, before it touches the screen.

What's different on iOS:

- A build runs on iOS 18 and later, on the iPhone XS and XR and every iPhone since. A release build counts on the A12's processor, and one for the simulator on the M1's.
- An app runs alone in its sandbox, which lets it start no other program, so [std.process](../std/process.md#ios) fails with `Unsupported` there, and on the simulator too, so what works on one works on the other.
- [std.keyring](../std/keyring.md) keeps its secrets in the app's own Keychain, which needs the application identifier every app Xcode builds is given.
- std.net's event loop is a kqueue, as on macOS. [std.http](../std/http.md)'s requests go through URLSession, and a [WebSocket](../std/http-websocket.md)'s connection through Network.framework, where Linux and macOS use libcurl, and the library links both frameworks by itself. The app's App Transport Security applies to the requests.
- The runtime calls none of Apple's private functions, which the App Store turns an app away for: its threads wait with `os_sync_wait_on_address`.

`calx build --target aarch64-ios-sim` builds a program too, which `xcrun simctl spawn booted ./program` runs on a simulator that's started, for trying code there without an app. A program for an iPhone needs an app around it, and its signing, to run, so for an iPhone a static library is what's built.

## How it's built

- The library is built from the same C as a program, with the runtime's parts the code uses, and archived with `llvm-lib` on Windows, `libtool` on macOS and for iOS, and `llvm-ar` or `ar` on Linux. An XCFramework is two such libraries, built one after the other from the same C. `--verbose` prints the commands.
- A release build isn't built with link-time optimization, since the host's linker, not clang's, links it. On Linux and macOS the code is position-independent, as a Rust or Swift host's executables are.
- Each `@export_c` function is a small C function of the name it's exported as, which checks that the runtime has started and calls the Calx function. Every other name in the library starts with `cx_` or `calx_`, so it doesn't meet the host's.
- `--cdylib`, a shared library with the same C, isn't built yet.
