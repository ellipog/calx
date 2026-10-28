---
title: Unsafe code and C functions
description: unsafe blocks, raw pointers and their methods, declaring and calling C functions, and their C.
---

# Unsafe code and C functions

Calx is memory-safe outside `unsafe` (D23). Inside an `unsafe { }` block, code can use raw pointers and call C functions, which the compiler can't check. Calcite's core uses it to reach the operating system, and `std` uses it underneath safe functions such as [`std.io`](../std/io.md).

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

Which modules may use `unsafe` and `extern` isn't enforced yet. The rule, that only Calcite core and trusted plugins may (D15, D23), comes with Calcite's plugin loader.

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

A pointer is only ever a value of its own, inside `unsafe`:

- Any expression whose value is a pointer must be inside `unsafe`, so a variable that holds one is declared there too, with its type written as `*T` if needed.
- A pointer can't be inside another type yet: not in a struct field, a list, an array, a tuple or an optional, and not as a generic parameter.
- Only a C function can take or return one. A Calx function's parameters and result can't be pointers.
- Pointers are `Copy`, and compare with `==` and `!=`.

| Method | Gives |
| --- | --- |
| `p.read()` | The value `p` points at. `T` must be `Copy` |
| `p.write(v)` | Stores `v` where `p` points. `p` must be a `*T`, and `T` `Copy` |
| `p.add(n)` | A pointer `n` elements further on, `n` a `usize` |
| `p.is_null()` | Whether `p` is null |

`read` and `write` copy whole values, so they need `Copy` types: copying a value that owns memory would leave two owners of it.

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

`xs.set_len(n)` sets a list's length without making or dropping elements, for code that has written them through a pointer, such as a C function that reads into a buffer. It needs `unsafe`, since the elements up to `n` must have been written, and elements that are `Copy`. A length past the capacity panics.

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
- Its parameters and result are C types: integers, floats, `bool`, and pointers to them. It can return nothing.
- Its parameters are plain values, so they can't be `mut` or `own`. Pass a pointer instead.
- Calling it needs `unsafe`.
- Like any function, it's private to its module unless `pub`, and one that's never used is an error.
- A C function has one signature. Two modules can both declare it, but the same way, or it's an error.

The only foreign ABI is `"C"`. Calling Calx from C (`@export_c`) and `@repr(c)` structs are designed but not built ([spec](../CALX_SPEC.md#ffi)).

## In C

- A pointer is `T*` or `T const*`, and its methods are plain C: `*p`, `*p = v`, `p + n`, `p == 0`.
- A C function is declared in each C file that calls it, under the name `cx_c_name`, with an asm label giving the linker its real name: `extern cx_i32 cx_c_abs(cx_i32) __asm__(CX_SYMBOL("abs"));`. So a declaration never meets one a header makes, and two declarations of the same function, which must agree, are the same C.
- `unsafe` adds nothing to the C. It's a promise checked by the reader, not by the program.
