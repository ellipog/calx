---
title: Interfaces
description: Interfaces with methods, static functions, constants and defaults; impls for structs, enums and primitives; generic interfaces such as Into<T>; and calls that name their interface.
---

# Interfaces

An interface names functions and constants that a type can have. A type has them once it implements the interface, and generic code can use them through a [bound](generics.md#bounds).

```calx
interface Shape {
    fn area(self) -> f64
    fn name() -> str
    const sides: i32

    fn size(self) -> str {
        return if self.area() > 10.0 { "big" } else { "small" }
    }
}

struct Square {
    side: f64
}

impl Shape for Square {
    fn area(self) -> f64 {
        return self.side * self.side
    }
    fn name() -> str {
        return "square"
    }
    const sides = 4
}

fn report<T: Shape>(s: T) {
    println("{T.name()}: {s.area()}, {T.sides} sides, {s.size()}")
}

fn main() {
    report(Square { side: 2.0 })    // out: square: 4.0, 4 sides, small
    println("{Square.sides}")       // out: 4
}
```

There's no inheritance and there are no classes. A type gets behavior from its own `impl` blocks and from the interfaces it implements.

## Declaring one

- `fn m(self, ...)` is a method, called on a value: `s.area()`.
- A function without `self` is called on a type: `T.name()`, `Square.name()`.
- `const name: Type` is a constant each impl gives. It's a constant wherever it's used, so `T.sides` can be a field's default.
- A function with a body is a default, as `size` is above. Impls get it without writing it, and can replace it.
- `Self` is the type implementing the interface: `fn bigger(self, other: Self) -> bool`, `fn unit() -> Self`.

## Implementing one

`impl Shape for Square { ... }` gives every member that has no default:

- Each function matches the interface's exactly, parameter names included, with `Self` replaced by the type. Since the interface fixes the names, an impl that doesn't use a parameter isn't an error.
- A constant can leave its type out, since the interface gives it.
- An impl has only the interface's members. Anything else goes in a separate `impl Square` block.
- Structs, enums and the primitive types (integers, floats, `bool`, `char`, `rune` and `str`) can implement interfaces.
- A type implements an interface at most once.
- A type has one function of each name, as in Java. Its own `impl` blocks can't have a function named like one an interface gives it, a default included. To replace a default, put the function in the interface's impl.
- An impl for a generic type can have bounds of its own. `impl<T: Code> Code for Wrapper<T>` gives `Wrapper<T>` a code whenever `T` has one.
- `Equal`, `Display`, `Hash`, `Order` and `JavaHash` are interfaces too, built in, which a struct or an enum implements to write its own property ([comparing and printing](comparing.md#writing-your-own)).

```calx
interface Code {
    fn code(self) -> i64
}

impl Code for i32 {
    fn code(self) -> i64 {
        return i64(self) * 10
    }
}

impl Code for bool {
    fn code(self) -> i64 {
        return if self { 1 } else { 0 }
    }
}

struct Wrapper<T> {
    inner: T
    extra: i64 = 0
}

impl<T: Code> Code for Wrapper<T> {
    fn code(self) -> i64 {
        return self.inner.code() + self.extra
    }
}

fn main() {
    let w = Wrapper { inner: 5, extra: 2 }
    println("{7.code()} {true.code()} {w.code()} {Wrapper { inner: w }.code()}")    // out: 70 1 52 52
}
```

A mismatch says what the interface declares:

```calx
interface Grow {
    fn grow(self, by: f64) -> f64
}

struct Tree {
    height: f64
}

impl Grow for Tree {
    fn grow(self, amount: f64) -> f64 {    // error: `grow` doesn't match `Grow`, which declares it as `fn grow(self, by: f64) -> f64`
        return self.height + amount
    }
}

fn main() {
    println("{Tree { height: 1.0 }.grow(2.0)}")
}
```

## Calling one

Generic code calls through its bounds, and other code calls on the type. A call in generic code always means the bound's function, in every copy. When two interfaces a type implements both have the name, `x.go()` is an error, and the interface's name says which one to call:

```calx
interface Up {
    fn go(self) -> i32
}

interface Down {
    fn go(self) -> i32
}

struct Lift {
    floor: i32
}

impl Up for Lift {
    fn go(self) -> i32 {
        return self.floor + 1
    }
}

impl Down for Lift {
    fn go(self) -> i32 {
        return self.floor - 1
    }
}

fn main() {
    let l = Lift { floor: 5 }
    println("{Up.go(l)} {Down.go(l)}")    // out: 6 4
}
```

## Generic interfaces

An interface can have parameters of its own, after `Self`. Each use gives them, and each list of arguments is a separate interface, so a type can implement `Into<i64>` and `Into<String>` both (D103):

```calx
interface Into<T> {
    fn into(self) -> T

    fn twice(self) -> List<T> {
        return [self.into(), self.into()]
    }
}

struct Meters {
    v: i32,
}

impl Into<i64> for Meters {
    fn into(self) -> i64 {
        return i64(self.v) * 1000
    }
}

impl Into<String> for Meters {
    fn into(self) -> String {
        return "{self.v} m"
    }
}

interface Scale<T> {
    fn scale(self, by: T) -> Self
}

impl Scale<i32> for Meters {
    fn scale(self, by: i32) -> Self {
        return Meters { v: self.v * by }
    }
}

impl Scale<Meters> for Meters {
    fn scale(self, by: Meters) -> Self {
        return Meters { v: self.v * by.v }
    }
}

// The bound names the arguments, and can mention other parameters, as in `B: From<A>`.
fn total<T: Into<i64>>(xs: List<T>) -> i64 {
    var sum = 0i64
    for x in xs {
        let n: i64 = x.into()
        sum += n
    }
    return sum
}

fn main() {
    let m = Meters { v: 3 }
    let a: i64 = m.into()
    let s: String = m.into()
    println("{a} {s}")    // out: 3000 3 m
    println("{m.scale(2).v} {m.scale(Meters { v: 4 }).v}")    // out: 6 12
    let both: List<Meters> = [m, Meters { v: 1 }]
    println("{total(both)}")    // out: 4000
}
```

- When a type implements an interface for more than one list of arguments, a call chooses between the impls the way a generic function's call works out its types: from the arguments, then from where the result goes. `m.scale(2)` takes `Scale<i32>`, and `let a: i64 = m.into()` takes `Into<i64>`. A call that neither settles is an error, as a generic call is.
- A generic interface is always written with its arguments: a bound `T: Into` is an error that asks for them.
- An impl for a generic type can use its parameters in the arguments: `impl<A: Copy> Into<List<A>> for Pair<A>`.
- Defaults can use the interface's parameters, as `twice` does.
- Every parameter must be used by a member, or no use could tell what it is.
- [`From<T>`](errors.md#converting-errors) is a built-in generic interface, which `try` converts errors with.

## Errors

Besides mismatched and missing members (D72):

- Implementing the same interface twice for a type, or a generic interface twice with the same arguments.
- A member that isn't in the interface.
- An interface used as a type, as in `fn f(s: Shape)`. Take a generic parameter with it as a bound instead: `fn f<T: Shape>(s: T)`.
- An impl that's never used, when neither the interface nor the type is `pub`.

## In C

Interface calls are direct calls. Generic code has a copy for each type, and each impl's functions are plain C functions named for the type and the interface, such as `Lift__Up__go`, and for a generic interface's arguments too, such as `Meters__Into_i64__into`. Constants are inlined. Nothing is looked up at run time, even when a call chooses between impls: the checker chooses.

`dyn Shape`, for holding values of different types together, is designed but not built yet, along with associated types and the operator interfaces ([spec](../CALX_SPEC.md#interfaces-and-generics)).
