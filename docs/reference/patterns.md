---
title: Patterns and match
description: match, every kind of pattern, exhaustiveness, if let, let-else and destructuring.
---

# Patterns and `match`

A pattern describes the shape of a value, and binds names to its parts. `match` picks the first arm whose pattern fits:

```calx
enum Event {
    Join(str)
    Move { dx: i32, dz: i32 }
    Leave
}

fn describe(e: Event) -> str {
    return match e {
        Join(name) => name,
        Move { dx: 0, dz: 0 } => "stand",
        Move { dx, dz } if dx == dz => "diagonal",
        Move { .. } => "move",
        Leave => "leave",
    }
}

fn main() {
    println("{describe(Event.Join("Alex"))} {describe(Event.Move { dx: 0, dz: 0 })}")   // out: Alex stand
    println("{describe(Event.Move { dx: 2, dz: 2 })} {describe(Event.Leave)}")          // out: diagonal leave
}
```

## `match`

`match value { pattern => body, ... }` runs `value` once, then tries the arms from the top. Arms are separated by commas or new lines.

- **A guard**, `pattern if cond => body`, also needs `cond` to be true. `cond` can use the names the pattern bound.
- **A body** is an expression, a block, or a statement that makes sense alone: `return`, `break`, `continue` or an assignment such as `total += n`.
- **As a value**, every arm gives the same type, or leaves. As a statement, each arm's body is `()`.

```calx
fn main() {
    var total = 0
    for n in 0..10 {
        match n {
            3 => continue,
            7 => break,
            x if x % 2 == 0 => total += x * 10,
            _ => total += n,
        }
    }
    println("{total}")   // out: 126
}
```

## Every case, every arm (D63)

A `match` must cover every value its input can have, and every arm must be able to match something the arms above it don't. Both are errors, and the error shows a value that's missing:

```calx
enum Color { Red, Green, Blue }

fn f(c: Color, n: u8, o: i32?) {
    match c {                       // error: doesn't cover `Color.Blue`
        Red | Green => println("warm"),
    }
    match n {                       // error: doesn't cover `101..=199`
        0..=100 => println("low"),
        200..=255 => println("high"),
    }
    match o {                       // error: doesn't cover `some(_)`
        some(v) if v > 0 => println("{v}"),
        none => println("none"),
    }
    match c {
        _ => println("any"),
        Red => println("red"),      // error: this arm never matches
    }
}

fn main() {
    f(Color.Red, 0u8, none)
}
```

- An arm with a guard covers nothing, since the guard might be false.
- `_` at the end covers the rest. Prefer naming the cases when there are few, so a new variant is an error until it's handled.
- Integers are covered by ranges, so `0..=127` and `-128..=-1` cover an `i8`. A `bool` is covered by `true` and `false`.

## Kinds of pattern

| Pattern | Matches |
| --- | --- |
| `_` | Anything |
| `name` | Anything, binding a copy of it to `name`. The name starts with a lowercase letter, and isn't already in use ([no shadowing](variables.md#no-shadowing)) |
| `5`, `'a'`, `"help"`, `true` | Equal values: integers, `char`, `rune`, `str` and `bool`. Not floats; compare those with a guard |
| `LIMIT`, `i32.MAX` | A constant's value |
| `1..10`, `'a'..='z'`, `i32.MIN..=-1` | A range, with or without its end. The ends are literals or constants, and the range can't be empty |
| `Circle(r)`, `Shape.Circle(r)` | A variant and its values. The enum's name is optional, since the input's type says which enum it is |
| `Rect { w, h }`, `Rect { w: 0.0, .. }` | A variant with fields. `w` alone binds `w`, `w: p` matches `p`, and `..` skips the fields not named |
| `Empty` | A variant without data |
| `some(p)`, `none` | An optional that holds a value matching `p`, or is empty |
| `ok(p)`, `err(p)` | The result of a call that can fail: its value matching `p`, or its error matching `p`. `ok` is written alone when the function returns nothing ([Errors](errors.md#matching-ok-and-err)) |
| `(p, q)` | A tuple, part by part |
| `p \| q` | Either. The alternatives can't bind names yet |

Patterns nest: `some(Move { dx: 0, .. })`, `(Red, 1..=9)`.

```calx
const SPAWN: i32 = 100

fn size(n: i32) -> str {
    return match n {
        i32.MIN..=-1 => "negative",
        0 => "zero",
        1..10 => "small",
        SPAWN => "spawn",
        _ => "big",
    }
}

fn command(s: str) -> i32 {
    return match s {
        "help" => 1,
        "stop" | "quit" => 2,
        _ => 0,
    }
}

fn quadrant(p: (i32, i32)) -> i32 {
    return match p {
        (0, _) | (_, 0) => 0,
        (1..=i32.MAX, 1..=i32.MAX) => 1,
        (i32.MIN..=-1, 1..=i32.MAX) => 2,
        (i32.MIN..=-1, i32.MIN..=-1) => 3,
        _ => 4,
    }
}

fn main() {
    println("{size(-3)} {size(0)} {size(9)} {size(100)} {size(500)}")   // out: negative zero small spawn big
    println("{command("quit")} {command("x")}")                          // out: 2 0
    println("{quadrant((0, 5))} {quadrant((-2, 3))} {quadrant((4, -4))}")  // out: 0 2 4
}
```

### Names in patterns

A name that starts with a capital letter is a variant or a constant, never a new binding, so a misspelled variant can't silently match everything as it would in Rust. A capitalized name that's neither is an error. A record pattern that leaves out a field without `..` is an error too, so adding a field shows every pattern that should look at it.

```calx
enum Color { Red, Green }

enum Shape {
    Circle(f64)
    Rect { w: f64, h: f64 }
}

fn f(c: Color, s: Shape) {
    match c {
        Purple => println("purple"),        // error: `Purple` isn't a variant of `Color`
        _ => println("other"),
    }
    match s {
        Rect { w } => println("{w}"),       // error: leaves out field `h`
        _ => println("other"),
    }
}

fn main() {
    f(Color.Red, Shape.Circle(1.0))
}
```

A pattern binds copies, so `mut name` isn't allowed. To change the value, copy it into a `var`.

## `let` and `for`

`let`, `var` and `for` take a pattern too, which must match every value of its type:

```calx
fn main() {
    let (a, b) = (1, 2)
    var (x, _) = (3, 4)
    x += a + b
    for (i, ch) in [(1, 'a'), (2, 'b')] {
        print("{i}{ch};")
    }
    println("{x}")   // out: 1a;2b;6
}
```

## `let ... else`

`let pattern = value else <leave>` binds the names when the pattern fits, and runs the `else` when it doesn't. The `else` must leave: `return`, `break`, `continue` or a panic. On an optional, the pattern matches the value inside, so `let x = opt else return` unwraps it.

```calx
enum Shape {
    Circle(f64)
    Empty
}

fn radius(s: Shape) -> f64 {
    let Circle(r) = s else {
        return 0.0
    }
    return r
}

fn main() {
    println("{radius(Shape.Circle(1.5))} {radius(Shape.Empty)}")   // out: 1.5 0.0
}
```

A pattern that matches every value doesn't take an `else`, and one that doesn't needs it:

```calx
enum Shape {
    Circle(f64)
    Empty
}

fn f(s: Shape, o: i32?) {
    let Circle(r) = s                   // error: doesn't match every `Shape`, such as `Shape.Empty`
    let (a, b) = (1, 2) else return     // error: matches every `(i32, i32)`; remove the `else`
    let x = o else {                    // error: must leave
        println("no")
    }
    println("{r} {a} {b} {x}")
}

fn main() {
    f(Shape.Empty, none)
}
```

## `if let`

`if let pattern = value { } else { }` runs the first block when the pattern fits, with its names bound only there. Like `let ... else`, on an optional it matches the value inside. It chains with `else if` and `else if let`, and gives a value like any `if`:

```calx
enum Shape {
    Circle(f64)
    Empty
}

fn half(n: i32) -> i32? {
    if n % 2 == 0 {
        return some(n / 2)
    }
    return none
}

fn pick(n: i32) -> Shape? {
    if n > 0 {
        return some(Shape.Circle(f64(n)))
    }
    return none
}

fn main() {
    for n in 0..3 {
        if let Circle(r) = pick(n) {
            print("{r};")
        } else {
            print("none;")
        }
    }
    println()                            // out: none;1.0;2.0;
    for k in 5..8 {
        let w = if let h = half(k) { h } else if let q = half(k + 1) { q * 10 } else { 0 }
        print("{w};")
    }
    println()                            // out: 30;3;40;
}
```

A pattern that always matches is an error after `if let`: write `let`.

## In C

A `match` compiles to a chain of tests. A variable or field is tested in place; anything else, or an input with guards, is computed once into a temporary. The last arm is never tested, since coverage proves it matches, and bindings are plain copies of the parts.
