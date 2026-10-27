---
title: Generics
description: Generic functions, structs and enums, how a call works out its types, bounds, and what generic code compiles to.
---

# Generics

A generic function or type takes types as parameters, written in `<...>` after its name. Each use with real types gets its own copy in C, so generic code runs as fast as code written for one type.

```calx
fn choose<T>(c: bool, a: own T, b: own T) -> T {
    return if c { a } else { b }
}

struct Pair<A, B> {
    first: A
    second: B
}

fn main() {
    println("{choose(true, 1, 2)} {choose(false, "x", "y")}")    // out: 1 y
    let p = Pair { first: 7, second: "seven" }
    println("{p.first} {p.second}")                               // out: 7 seven
}
```

## Generic functions

`fn name<T, U>(...)` declares the parameters. A call usually works them out from its arguments ([below](#how-a-call-works-out-its-types)), and can also give them, right after the name: `choose<i64>(true, 7, 8)`.

```calx
fn choose<T>(c: bool, a: own T, b: own T) -> T {
    return if c { a } else { b }
}

fn first<T: Copy>(xs: [T; 3]) -> T {
    return xs[0]
}

fn swap<A, B>(t: own (A, B)) -> (B, A) {
    let (a, b) = t
    return (b, a)
}

fn main() {
    println("{choose<i64>(true, 7, 8)} {first(['a', 'b', 'c'])}")   // out: 7 a
    let (n, s) = swap(("one", 1))
    println("{n} {s}")                                               // out: 1 one
}
```

- In an expression, `<` right after a name, with no space, starts its types. With a space it's less-than ([syntax](syntax.md#generic-types-in-expressions)).
- Every parameter must be used. A parameter can't take the name of a type or of anything else in the module, since there's no shadowing.
- A generic function can call itself and other generic functions.
- `T` might own memory, so its values move rather than copy, and parameters that keep one are `own` ([below](#values-of-type-t)).

## Generic structs and enums

```calx
struct Pair<A, B> {
    a: A
    b: B
}

impl<A, B> Pair<A, B> {
    fn swap(own self) -> Pair<B, A> {
        return Pair { a: self.b, b: self.a }
    }
    fn with<C>(own self, c: own C) -> Pair<A, C> {
        return Pair { a: self.a, b: c }
    }
    fn of(a: own A, b: own B) -> Pair<A, B> {
        return Pair { a, b }
    }
}

impl Pair<i32, i32> {
    fn sum(self) -> i32 {
        return self.a + self.b
    }
}

enum Either<L, R> {
    Left(L)
    Right(R)
    Neither
}

fn left_or<L, R>(e: own Either<L, R>, other: own L) -> L {
    return match e {
        Left(x) => x,
        _ => other,
    }
}

fn main() {
    let p = Pair { a: 1, b: "one" }              // a `Pair<i32, str>`
    let q = p.swap().with(2.5)
    println("{q.a} {q.b}")                         // out: one 2.5
    let r = Pair<i64, u8>.of(5, 6)
    let s: Pair<i32, i32> = Pair.of(2, 3)
    println("{r.a} {r.b} {s.sum()}")               // out: 5 6 5

    let e: Either<i32, str> = Either.Left(4)
    let n = Either<i32, str>.Neither
    println("{left_or(e, 0)} {left_or(n, 0)}")     // out: 4 0
}
```

- A literal works its types out from its fields, or from the type it's given to. `Pair<i32, i64> { a: 1, b: 2 }` gives them.
- A variant without data, such as `Either.Neither`, has nothing to work them out from, so it needs the variable's type or `Either<i32, str>.Neither`.
- `impl<A, B> Pair<A, B>` adds methods to every `Pair`, and `impl Pair<i32, i32>` adds them only to that one. An impl's parameters must all appear in its type.
- A method can have its own parameters, as `with<C>` does. A static function is called on the type, with or without its types: `Pair.of(2, 3)` works them out.
- A field's default can use the parameters, as in `best: T? = none`.
- A type that holds itself has no end, generic or not, so `struct Node<T> { v: T, next: Node<T>? }` is an error.

## How a call works out its types

A call looks in this order:

1. The types written in `<...>`.
2. The value a method is called on.
3. The arguments, left to right, except untyped literals such as `1` and `none`, which wait for the others.
4. The type the result is expected to have.

```calx
fn choose<T>(c: bool, a: own T, b: own T) -> T {
    return if c { a } else { b }
}

fn empty<T>() -> T? {
    return none
}

fn main() {
    let a = choose(true, 1, 2i64)             // `1` waits, so `T` is `i64`
    let b = choose(false, none, some(4u8))    // `T` is `u8?`
    let c: i16? = empty()                     // from the expected type
    println("{a} {b ?? 0} {c ?? -1}")          // out: 1 4 -1
}
```

When nothing says what a parameter is, that's an error, never a guess:

```calx
fn empty<T>() -> T? {
    return none
}

fn main() {
    let x = empty()    // error: can't tell what `T` is in this call to `empty`; give it, as in `empty<...>(...)`
    _ = x
}
```

## Bounds

A parameter's bounds are the [interfaces](interfaces.md) its type must implement: `<T: Shape>`, or `<T: Shape + Code>` for more than one. A call checks that its types have them.

```calx
interface Shape {
    fn area(self) -> f64
}

struct Square {
    side: f64
}

impl Shape for Square {
    fn area(self) -> f64 {
        return self.side * self.side
    }
}

fn total<T: Shape>(a: T, b: T) -> f64 {
    return a.area() + b.area()
}

fn main() {
    println("{total(Square { side: 1.0 }, Square { side: 2.0 })}")    // out: 5.0
}
```

Generic code is checked once, where it's written, and it can use only what its bounds give it. Without a bound, a value of type `T` can be held, passed, returned, assigned, and put in arrays, tuples, optionals and generic types, but nothing more. Six bounds are built-in properties rather than interfaces, which every type that can have them has:

| Bound | Gives |
| --- | --- |
| `T: Copy` | Using a `T` copies it ([values of type `T`](#values-of-type-t)) |
| `T: Equal` | `==` and `!=` on `T` ([comparing and printing](comparing.md#equal)) |
| `T: Order` | `<`, `<=`, `>`, `>=`, `cmp` and sorting on `T`, and `==` too ([comparing and printing](comparing.md#order)) |
| `T: Display` | `{x}` in a string ([comparing and printing](comparing.md#display)) |
| `T: Hash` | `T` as the key of a `Map` or a `Set`, and `==` too ([maps and sets](maps.md#hash)) |
| `T: JavaHash` | `x.java_hash()`, and `T` as the key of a `JavaHashMap` or a `JavaHashSet`, and `==` too ([Java's hash maps](java-maps.md#javahash)) |

```calx
fn count<T: Equal>(xs: List<T>, x: T) -> usize {
    var n = 0usize
    for y in xs {
        if y == x {
            n += 1
        }
    }
    return n
}

fn main() {
    let xs: List<(i32, bool)> = [(1, true), (2, false), (1, true)]
    println("{count(xs, (1, true))}")    // out: 2
}
```

Arithmetic and conversions on a `T` need the standard interfaces that will say what they mean for each type, which aren't built yet.

A bound on a struct's or enum's parameter, as in `struct Holder<T: Shape>`, applies to every use of the type.

A [generic interface](interfaces.md#generic-interfaces) as a bound names its arguments, which can be the other parameters (D103):

```calx
struct Celsius {
    v: f64,
}

struct Kelvin {
    v: f64,
}

impl From<Celsius> for Kelvin {
    fn from(value: own Celsius) -> Self {
        return Kelvin { v: value.v + 273.15 }
    }
}

fn convert<A, B: From<A>>(a: own A) -> B {
    return B.from(a)
}

fn main() {
    let k: Kelvin = convert(Celsius { v: 10.0 })
    println("{k.v}")    // out: 283.15
}
```

## Values of type `T`

Generic code can't know whether `T` owns memory, as a `String` does, so it treats every `T` as a value that might ([ownership](ownership.md)):

- Assigning one, passing it to an `own` parameter, returning it, or putting it in a struct, tuple or list moves it. After that, the variable it was in can't be used.
- A parameter without `own` is a view of the caller's value, so it can be read and passed on, but not moved away. A function that keeps or returns a `T` it's given takes it as `own T`.
- `T: Copy` is a bound saying `T` owns no memory. Its values then copy, as numbers do, and an element can be read out of an array of them.
- `x.copy()` makes a copy that owns memory of its own, for any `T`.

```calx
fn first<T: Copy>(xs: [T; 3]) -> T {
    return xs[0]
}

fn pick<T>(c: bool, a: own T, b: own T) -> T {
    return if c { a } else { b }    // the one not picked is freed here
}

fn twice<T>(x: T) -> (T, T) {
    return (x.copy(), x.copy())
}

fn main() {
    println("{first([4, 5, 6])} {pick(false, String("kept"), String("chosen"))}")    // out: 4 chosen
    let (a, b) = twice(String("echo"))
    println("{a} {b}")                                                                 // out: echo echo
}
```

Without `Copy`, reading an element out moves it, which an array that's only being looked at can't allow:

```calx
fn first<T>(xs: [T; 3]) -> T {
    return xs[0]    // error: can't move an element out of `xs`; move a copy with `.copy()`, or give `T` the bound `Copy`
}

fn main() {
    println("{first([1, 2, 3])}")
}
```

## Optionals inside optionals

Written out, [optionals](optionals.md) don't nest, so `i32??` isn't a type. Through a parameter they do. `find` below returns a `T?`, and with `T` being `i32?` that's an optional holding an optional. `some(none)`, found but empty, then differs from `none`, not found:

```calx
fn find<T: Copy>(xs: [T; 2], i: usize) -> T? {
    if i < 2 {
        return some(xs[i])
    }
    return none
}

fn main() {
    let xs: [i32?; 2] = [some(1), none]
    for i in 0usize..3 {
        match find(xs, i) {
            some(some(v)) => print("{v};"),
            some(none) => print("empty;"),
            none => print("missing;"),
        }
    }
    println()                        // out: 1;empty;missing;
    let r = find(xs, 1)
    println("{(r ?? none) ?? 0}")    // out: 0
}
```

On an optional holding an optional, `none` on the right of `??` is the inner optional, so `r ?? none` takes one level off.

## Errors

Besides the type errors any code can have, these are errors (D70, D71):

- A parameter that's never used, as in `fn f<T>() -> i32`, or an impl parameter missing from the impl's type, as in `impl<T> Square`.
- The wrong number of types, as in `Pair<i32>`, or types given to something that isn't generic.
- A generic type written without its types where nothing works them out, as in `let p: Pair = ...`.
- Copies that never end. A function that calls itself with a bigger type, as in `grow(some(x), n - 1)` inside `grow<T>`, would need endless copies.

An error in a generic definition is reported once, where it's written, and its uses aren't checked again. An error that only some types have, such as a constant index that's out of bounds with one type's `T.sides`, is reported with a note saying which use needed that copy.

## In C

Each set of types makes one copy of the function or type, named after them. After the module's prefix, `choose<i64>` is `choose__i64` and `Pair<i32, str>` is `Pair_i32_str`. Only the copies a program uses are made, and those names are what profilers and debuggers show.

There's no boxing and no table of functions, so a generic call costs what a direct call costs, and clang can inline it. The front end checks each definition once and each copy's body once more, which keeps generic code quick to build (D64).
