---
title: Unsafe code and C functions
description: Code the compiler can't check - unsafe blocks, raw pointers and their methods - and declaring and calling C functions, and their C.
---

# Unsafe code and C functions

Calx is memory-safe outside `unsafe` (D23). Inside an `unsafe { }` block, code can use raw pointers and call C functions, which the compiler can't check. A program uses it to reach the operating system or a C library, and `std` uses it underneath safe functions such as [`std.io`](../std/io.md).

```calx
extern "C" fn abs(x: i32) -> i32

fn sum(xs: List<i32>) -> i32 {
    var total = 0
    unsafe {
        let p = xs.as_ptr()
        var i: usize = 0
        while i < xs.len() {
            total += p.add(i).read()
            i += 1
        }
    }
    return total
}

fn main() {
    let xs: List<i32> = [3, -4, 5]
    let n = unsafe { abs(sum(xs) - 10) }
    println("{n}")    // out: 6
}
```

## `unsafe` blocks

- `unsafe { }` is a block. Like any block, it can be a statement, or give a value: `let n = unsafe { abs(x) }`.
- Inside it, and only there, code can use pointers, call C functions and [`unsafe` functions](#unsafe-functions), call `set_len` on a list, and open and close a [library](libraries.md).
- A block that does none of these is an error, so every `unsafe` marks something unsafe. So is an `unsafe` inside another, which adds nothing.
- Its variables end with the block, as any block's do. A pointer can't leave it: the block's value can't be a pointer.

```calx
fn main() {
    unsafe {    // error: nothing in this block needs `unsafe`
        println("hi")
    }
}
```

Which modules may use `unsafe` and `extern` isn't enforced yet. The rule, that only a program's own code and the plugins it trusts may, comes with plugin loading (D15, D23).

## `unsafe` functions

`unsafe fn` declares a function that only `unsafe` code can call. It's for a safe-looking function whose caller must know something the compiler can't check, or that reaches what safe Calx mustn't see, such as [the clock](../std/time.md).

```calx
extern "C" fn abs(x: i32) -> i32

/// Callers promise `x` isn't `i32.MIN`, whose magnitude doesn't fit.
unsafe fn magnitude(x: i32) -> i32 {
    return unsafe { abs(x) }
}

fn main() {
    let n = unsafe { magnitude(-5) }
    println("{n}")    // out: 5
}
```

Calling it outside `unsafe` is an error:

```calx
unsafe fn magnitude(x: i32) -> i32 {
    return x.abs()
}

fn main() {
    _ = magnitude(-1)    // error: `magnitude` is an `unsafe` function, so calling it needs `unsafe`
}
```

- The body isn't `unsafe` itself: anything unsafe in it still goes in an `unsafe` block, so each unsafe operation is marked where it happens.
- It can be a method or a static function in an `impl` block, and `pub` like any function.
- An interface's functions can't be `unsafe`, and neither can a function that implements one, since an interface's functions can be called from anywhere, including generic code. `main` can't be `unsafe` either.

## Pointers

`*T` points at a `T` that can be read and written, and `*const T` at one that can only be read. In C they're `T*` and `T const*`.

Pointers are only used inside `unsafe`:

- Any expression whose value is a pointer must be inside `unsafe`, so a variable that holds one is declared there too, with its type written as `*T` if needed.
- A struct's field can be a pointer (CLX-332), so a struct can hold what C gave it, such as a library's handle. The struct is used anywhere, as any struct is; reading the field, or making the struct, is inside `unsafe`. A pointer can't be in a list, an array, a tuple, an optional or an enum's variant, or be a generic parameter.
- A function's parameters and result can be pointers when it's a [C function](#c-functions), an [`unsafe fn`](#unsafe-functions) or an [`@export_c`](#calling-calx-from-c) function, since only `unsafe` code calls the first two, and C the third. A safe function's can't.
- A `*T` goes where a `*const T` is wanted, as `T*` does in C. The other way needs a `cast`.
- Pointers are `Copy`, and compare with `==` and `!=`. So does a struct that holds one, by address.

| Method | Gives |
| --- | --- |
| `p.read()` | The value `p` points at. `T` must be `Copy` |
| `p.write(v)` | Stores `v` where `p` points. `p` must be a `*T`, and `T` `Copy` |
| `p.add(n)` | A pointer `n` elements further on, `n` a `usize` |
| `p.is_null()` | Whether `p` is null |
| `p.addr()` | `p`'s address, as a `usize` |
| `p.cast<U>()` | The same address as a pointer to a `U`, `*U` or `*const U` as `p` is, as C's `(U*)p` |
| `p.to_fn<F>()` | The same address as a C function, of the [`extern "C" fn` type](#c-function-values) `F`, for an address C gave, such as `dlsym`'s or `GetProcAddress`'s |
| `p.to_box()` | Takes back the box `b.into_ptr()` gave, from a `*T` |

`read` and `write` copy whole values, so they need `Copy` types: copying a value that owns memory would leave two owners of it.

`b.into_ptr()` on a [`Box<T>`](boxes.md) gives its value to C as a `*T`, which C keeps and hands back, and `p.to_box()` takes it back as the box it was, which frees it when it ends. It's how C holds a Calx value it can't read, such as the state of a [static library](embedding.md), as Rust's `Box::into_raw` and `Box::from_raw` do. Each pointer goes back once: a second `to_box` of one would free it twice.

```calx
struct Counter {
    n: i32
}

fn main() {
    unsafe {
        let p = Box(Counter { n: 41 }).into_ptr()
        var back = p.to_box()
        back.value.n += 1
        println("{back.value.n}")    // out: 42
    }
}
```

Pointers come from the values they point into:

| Method | Gives |
| --- | --- |
| `xs.as_ptr()` | A `*const T` to a list's first element |
| `xs.as_mut_ptr()` | A `*T` to a list's first element. Needs a variable declared with `var`, or a `mut` parameter |
| `s.as_ptr()` | A `*const u8` to a `String`'s or `str`'s first byte |

A list or string that owns no memory, such as an empty one, gives a null pointer. A pointer into a list is good until the list grows, shrinks or is freed; nothing checks that inside `unsafe`.

```calx
fn main() {
    let xs: List<u8> = [1, 2]
    _ = xs.as_ptr()    // error: this is a pointer, `*const u8`, which can only be used inside `unsafe`
}
```

## Filling a list from C

`xs.set_len(n)` sets a list's length without making or dropping elements, for code that has written them through a pointer, such as a C function that reads into a buffer. It needs `unsafe`, since the elements up to `n` must have been written, and elements that are `Copy`. A length past the capacity panics. Set the length before anything that can grow the list, such as `reserve` or `push`: growing keeps only the elements up to the length, so it copies what the list holds rather than all the room it had.

```calx
fn fill(xs: mut List<u8>, n: usize, byte: u8) {
    xs.reserve(n)
    unsafe {
        let start = xs.len()
        let p = xs.as_mut_ptr().add(start)
        var i: usize = 0
        while i < n {
            p.add(i).write(byte)
            i += 1
        }
        xs.set_len(start + n)
    }
}

fn main() {
    var xs: List<u8> = [1]
    fill(mut xs, 2, 7)
    println("{xs}")    // out: [1, 7, 7]
}
```

## C functions

`extern "C" fn name(params) -> T` declares a function written in C. An `extern "C" { }` block declares several.

```calx
extern "C" {
    fn memcmp(a: *const u8, b: *const u8, n: usize) -> i32
    fn memset(to: *u8, byte: i32, n: usize) -> *u8
}

fn main() {
    var xs: List<u8> = [0, 0, 0]
    let ys: List<u8> = [9, 9, 0]
    unsafe {
        _ = memset(xs.as_mut_ptr(), 9, 2)
        println("{memcmp(xs.as_ptr(), ys.as_ptr(), 3)}")    // out: 0
    }
}
```

- The function's name is its C name.
- It has no body, and can't be generic.
- Its parameters and result are C types: integers, floats, `bool`, pointers, and [C function types](#c-function-values). It can return nothing.
- Its parameters are plain values, so they can't be `mut` or `own`. Pass a pointer instead.
- Calling it needs `unsafe`.
- Like any function, it's private to its module unless `pub`, and one that's never used is an error.
- A C function has one signature. Two modules can both declare it, but the same way, or it's an error.

The only foreign ABI is `"C"`. `@repr(c)` structs are designed but not built ([spec](../CALX_SPEC.md#ffi)), so a struct goes to C as a pointer to memory C lays out.

## C function values

`extern "C" fn(params) -> T` is the type of a C function's address, C's `T (*)(params)` (CLX-333). Its parameters and result are C types, as a C function's are. A value of it is a C function, which a C function takes to call back, such as `qsort`'s comparison; an [`@export_c`](#calling-calx-from-c) function, which C can call; or an address C gave, with `p.to_fn<F>()`. Calling one is calling C, so it needs `unsafe`.

```calx
extern "C" {
    fn qsort(base: *u8, n: usize, size: usize, cmp: extern "C" fn(*const u8, *const u8) -> i32)
    fn abs(x: i32) -> i32
}

@export_c
fn by_byte(a: *const u8, b: *const u8) -> i32 {
    return unsafe { i32(a.read()) - i32(b.read()) }
}

fn main() {
    var xs: List<u8> = [9, 3, 7, 1]
    unsafe {
        qsort(xs.as_mut_ptr(), xs.len(), 1, by_byte)
    }
    println("{xs}")    // out: [1, 3, 7, 9]
    let magnitude: extern "C" fn(i32) -> i32 = abs
    println("{unsafe { magnitude(-6) }}")    // out: 6
}
```

A Calx function or a closure isn't one. C calls the address with nothing else, so a closure's captures would be lost, and a Calx function's errors and parameter modes aren't C's. What a callback needs goes through a pointer C hands back to it, as with C's own `void* user_data`:

```calx
fn twice(x: i32) -> i32 {
    return x * 2
}

fn main() {
    let f: extern "C" fn(i32) -> i32 = twice    // error: `twice` is a Calx function, and an `extern "C" fn(i32) -> i32` is a C one; mark `twice` `@export_c`, so C can call it
    _ = f
}
```

## Calling Calx from C

`@export_c` makes a function C can call, by its own name or the one the attribute gives: `@export_c("mylib_open")` (CLX-333). Its address is an [`extern "C" fn`](#c-function-values) value, and a [static library](embedding.md) exports it, in the C header made for it.

```calx
@export_c("lib_add")
fn add(a: i32, b: i32) -> i32 {
    return a + b
}

fn main() {
    let f: extern "C" fn(i32, i32) -> i32 = add
    println("{unsafe { f(2, 3) }}")    // out: 5
}
```

- Its parameters and result are C types, plain values, as a C function's are. Text and lists go as a pointer and a length.
- It can't fail, be generic, be `unsafe`, be `@track_caller` or `@export`, or never return. A panic in it ends the process, as it does in a program: C has no way to catch one.
- Its C name is a C identifier, not a word of C's such as `int`, not one starting with `cx_` or `calx_`, which are the runtime's, and no other function's.
- The function itself is still a Calx function, which Calx code calls as any other.

### Callbacks on C's threads

C can call an `@export_c` function on any thread: one Calx started, the host's own, or a C library's, such as an audio callback. What the runtime keeps for each thread, its allocator's heap and the errors std's calls leave, is made the first time a thread needs it, so a thread Calx didn't start runs Calx as one it did, and output from it takes the same lock as everyone's. What's for the whole process is set up once, by `main` in a program and by `calx_init` in a static library. A call before that, or after `calx_shutdown`, is refused: the runtime says which function C called and ends the process with status 101, rather than run Calx with nothing set up.

```text
calx: C called `core_start` before `calx_init`, so the runtime hasn't started; call `calx_init` first
```

The values a callback gets are C's, so nothing of Calx's is shared by it, except what a pointer it was handed points at. Code that gives a thread such a pointer vouches for what it points at: a struct whose fields hold pointers isn't [`Send`](parallel.md) until it says so with `unsafe impl Send for S {}`, which says the pointers may be used on another thread, as Rust's `unsafe impl Send` does. It's in the struct's own module, and has no body.

```calx
import std.thread.{Task}

struct Sink {
    on_event: extern "C" fn(i32)
}

unsafe impl Send for Sink {}

struct Worker {
    sink: Sink
}

impl Task for Worker {
    fn run(own self) {
        unsafe { (self.sink.on_event)(7) }
    }
}

@export_c
fn heard(n: i32) {
    println("heard {n}")
}

fn main() {
    let worker = Worker { sink: Sink { on_event: heard } }
    unsafe { Thread.spawn(worker).join() }    // out: heard 7
}
```

## Owning what C gave

A [`@handle`](ownership.md#handles) struct can have a `drop`, `fn drop(mut self)`, which runs once wherever the value ends: at the end of its scope, when its variable gets another value, when a list holding it is freed, or in the function it moved to (CLX-332). So a safe wrapper owns what a C library gave, such as `mpv_create`'s handle, and gives it back, as Rust's `Drop` does:

```calx
extern "C" {
    fn malloc(n: usize) -> *u8
    fn free(p: *u8)
}

@handle
struct Buffer {
    p: *u8
    name: String
}

impl Buffer {
    fn new(name: own String) -> Buffer {
        return unsafe { Buffer { p: malloc(16), name: name } }
    }

    fn drop(mut self) {
        println("freeing {self.name}")
        unsafe { free(self.p) }
    }
}

fn main() {
    var b = Buffer.new("first")
    b = Buffer.new("second")    // out: freeing first
    println("using {b.name}")    // out: using second
}
// out: freeing second
```

- Only a `@handle` struct has one, since a value that could be copied would be dropped once for each copy. It's never called by hand: to end a value early, pass it to a function that takes it with `own`.
- A value moved away isn't dropped where it was, and one whose fields are all zero, such as one that never got a handle, isn't dropped at all.
- A pattern can't take a struct with a `drop` apart, since `drop` runs on the whole value. Read its fields instead.
- A generic struct can't have one yet.

## In C

- A pointer is `T*` or `T const*`, and its methods are plain C: `*p`, `*p = v`, `p + n`, `p == 0`, `(U*)p`.
- A C function is declared in each C file that calls it, under the name `cx_c_name`, with an asm label giving the linker its real name: `extern cx_i32 cx_c_abs(cx_i32) __asm__(CX_SYMBOL("abs"));`. So a declaration never meets one a header makes, and two declarations of the same function, which must agree, are the same C.
- An `extern "C" fn` type is a C function pointer, and calling one is `f(x)`.
- An `@export_c` function is its Calx function, called by a small C function under the C name, which checks that the runtime has started and calls it.
- `unsafe` adds nothing to the C. It's a promise checked by the reader, not by the program.
