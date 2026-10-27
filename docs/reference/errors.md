---
title: Errors
description: Functions that can fail, fail, try, matching ok and err, unwrapping with !, and their C.
---

# Errors

A function that can fail says what it fails with, after `!`. Its errors are values that the caller must handle where it makes the call. Calx has no exceptions, and a panic is for bugs ([Printing and panics](printing.md#panics)).

```calx
enum ParseError {
    Empty
    TooLong(usize)
}

fn parse_name(raw: own String) -> String ! ParseError {
    if raw.is_empty() {
        fail ParseError.Empty
    }
    if raw.len() > 16 {
        fail ParseError.TooLong(raw.len())
    }
    return raw
}

fn greet(raw: own String) -> String ! ParseError {
    let name = try parse_name(raw)    // passes the error up
    return "hi {name}"
}

fn main() {
    for raw in ["steve", "", "a name far too long to fit"] {
        match greet(String(raw)) {
            ok(text) => println(text),
            err(ParseError.Empty) => println("no name"),
            err(TooLong(n)) => println("{n} letters is too long"),
        }
    }
    // out: hi steve
    // out: no name
    // out: 26 letters is too long
}
```

## Failing

`-> T ! E` means the function returns a `T` or fails with an `E`. `fn f() ! E` returns nothing, but can fail.

- `return v` gives back the value, and `fail e` returns the error `e`.
- A function that returns nothing gives back success when it reaches its end, as it does without `!`.
- The error can be any type but `()`. An enum is the usual choice, since the caller can match each case. A `String` suits an error that's only a message.
- A `pub` function can fail only with a type other modules can name, as for what it returns.
- A C function can't fail. Declare what it returns, and check that in Calx.

```calx
fn check(health: i32) ! String {
    if health < 0 {
        fail "health can't be {health}"
    }
}

fn main() {
    if let err(message) = check(-3) {
        println(message)    // out: health can't be -3
    }
}
```

## Handling an error

The call to a function that can fail gives a result, `T ! E`: either `ok` with the value, or `err` with the error. A result is handled at the call, in one of these ways:

| Written | Does |
| --- | --- |
| `try f()` | `f`'s value. If `f` fails, the function returns the same error |
| `match f() { ok(v) => ..., err(e) => ... }` | Runs the arm that matches. Every case must be covered |
| `if let ok(v) = f() { }` | Runs the block if `f` succeeds. `if let err(e) = f()` runs it if `f` fails |
| `let ok(v) = f() else { return }` | Binds the value, or runs the `else`, which must leave |
| `f()!` | `f`'s value. Panics if `f` fails |

A result can't be put in a variable, passed to a function or returned. `_ =` can't discard it either. So an error never slips by unseen, and the code always says what happens to it ([Strictness](strictness.md#nothing-is-dropped-silently)).

```calx
fn parse(n: i32) -> i32 ! String {
    if n < 0 {
        fail "negative"
    }
    return n
}

fn main() {
    let r = parse(3)    // error: `parse` can fail, so handle its error here
    println("{r}")
}
```

## `try`

`try f()` is `f`'s value when `f` succeeds. When it fails, the function returns that error at once, and everything it owns is dropped, as a `return` there would drop it. The function must fail with the same error type as `f`. `try` can go anywhere a value can, even inside another call:

```calx
struct Reader {
    bytes: List<u8>,
    at: usize,
}

impl Reader {
    fn byte(self: mut Reader) -> u8 ! String {
        if self.at >= self.bytes.len() {
            fail "ran out at byte {self.at}"
        }
        let b = self.bytes[self.at]
        self.at += 1
        return b
    }

    // A protocol VarInt: 7 bits a byte, low bits first.
    fn varint(self: mut Reader) -> i32 ! String {
        var value = 0
        var shift = 0
        loop {
            let b = try self.byte()
            value |= (i32(b) & 0x7f) << shift
            if b & 0x80 == 0 {
                return value
            }
            shift += 7
            if shift >= 35 {
                fail "VarInt too long"
            }
        }
    }
}

fn sum(r: mut Reader) -> i32 ! String {
    return (try r.varint()) + (try r.varint())
}

fn main() {
    var r = Reader { bytes: [0xac, 0x02, 0x05, 0x01], at: 0 }
    println("{sum(mut r)!}")    // out: 305
    match sum(mut r) {
        ok(n) => println("{n}"),
        err(e) => println(e),    // out: ran out at byte 4
    }
}
```

`try` in a function that can't fail is an error, since there's nowhere to pass the error to. Handle it with `match` there.

## Matching `ok` and `err`

`ok(p)` and `err(p)` match a result, and `p` can be any pattern, such as `err(ParseError.TooLong(n))` or `ok(0)`. A `match` must cover both, and every case of each ([Patterns](patterns.md)). When the function returns nothing, `ok` holds nothing and is written alone:

```calx
fn save(slot: i32) ! String {
    if slot > 8 {
        fail "no slot {slot}"
    }
}

fn main() {
    match save(2) {
        ok => println("saved"),    // out: saved
        err(e) => println(e),
    }
}
```

`ok(...)` and `err(...)` aren't values: a function gives back its value with `return` and its error with `fail`.

## `!`

`f()!` is `f`'s value, and panics if `f` fails, as `!` does on `none`. It suits errors that can't happen in a correct program, and tests.

```calx
fn half(n: i32) -> i32 ! String {
    if n % 2 != 0 {
        fail "{n} is odd"
    }
    return n / 2
}

fn main() {
    println("{half(8)!}")    // out: 4
    println("{half(3)!}")    // panic: unwrapped an error with `!`
}
```

## In a `parallel for`

Every iteration of a [`parallel for`](parallel.md) runs, so its body can't `fail` or pass an error up with `try`. Each iteration keeps its own outcome instead, in its own element:

```calx
fn parse(n: i32) -> i32 ! String {
    if n < 0 {
        fail "negative"
    }
    return n * 2
}

fn main() {
    let xs: List<i32> = [1, -2, 3]
    var doubled = [0; 3]
    var bad = [false; 3]
    parallel for k in 0usize..xs.len() {
        match parse(xs[k]) {
            ok(v) => doubled[k] = v,
            err(_) => bad[k] = true,
        }
    }
    println("{doubled[2]} {bad[1]}")    // out: 6 true
}
```

## In C

A result is a C struct, like an [enum with data](enums.md): a one-byte tag, 0 for `ok` and 1 for `err`, and a union holding the value or the error. A small one, such as `u8 ! ReadError`, is two bytes, so it comes back in a register. `try` is a branch that's marked unlikely. It holds the error's return, after the drops a `return` there would make. For example:

```calx
enum ReadError {
    End
}

fn byte(bytes: List<u8>, at: usize) -> u8 ! ReadError {
    if at >= bytes.len() {
        fail ReadError.End
    }
    return bytes[at]
}

fn short(bytes: List<u8>) -> i32 ! ReadError {
    let hi = try byte(bytes, 0)
    let lo = try byte(bytes, 1)
    return (i32(hi) << 8) | i32(lo)
}

fn main() {
    let bytes: List<u8> = [1, 2]
    println("{short(bytes)!}")    // out: 258
}
```

becomes, with `#line` directives left out:

```c
struct cx_Result_u8_ReadError { cx_u8 tag; union { struct { cx_u8 f0; } v0; struct { game__ReadError f0; } v1; } u; };

static cx_Result_i32_ReadError game__short(const cx_list_u8* restrict bytes) {
    CX_VIEW(bytes);
    cx_u8 hi = ({ cx_Result_u8_ReadError cx_t1 = game__byte(&(*bytes), ((cx_usize)0ULL)); if (__builtin_expect(cx_t1.tag != ((cx_u8)0u), 0)) {
        cx_Result_i32_ReadError cx_t2 = ((cx_Result_i32_ReadError){ .tag = ((cx_u8)1u), .u.v1 = { .f0 = cx_t1.u.v1.f0 } });
        return cx_t2;
    } cx_t1.u.v0.f0; });
    cx_u8 lo = ({ cx_Result_u8_ReadError cx_t3 = game__byte(&(*bytes), ((cx_usize)1ULL)); if (__builtin_expect(cx_t3.tag != ((cx_u8)0u), 0)) {
        cx_Result_i32_ReadError cx_t4 = ((cx_Result_i32_ReadError){ .tag = ((cx_u8)1u), .u.v1 = { .f0 = cx_t3.u.v1.f0 } });
        return cx_t4;
    } cx_t3.u.v0.f0; });
    return ((cx_Result_i32_ReadError){ .tag = ((cx_u8)0u), .u.v0 = { .f0 = ((cx_i32)(((cx_i32)((cx_u32)((cx_i32)hi) << 8)) | ((cx_i32)lo))) } });
}
```

When an earlier part of the same expression made a value that's owned, such as the `name.copy()` in `pair(name.copy(), try read())`, the value waits in a temporary of the statement. The `try` is worked out before the call, and the temporary is cleared only as the value moves into it. So passing the error up drops the value, and the normal path doesn't drop it twice, whatever order C evaluates the arguments in.

Still to build, in the [spec](../CALX_SPEC.md#errors-are-values): converting an error to the function's error type through `From`, `fn main() ! E`, and `! E` in function types.
