---
title: Interfaces
description: Interfaces with methods, static functions, constants and defaults; impls for structs, enums and primitives; and calls that name their interface.
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
- An impl for a generic type can have bounds of its own. `impl<T: Code> Code for Wrapper<T>` gives `Wrapper<T>` a code whenever `T` has one.

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

Generic code calls through its bounds, and other code calls on the type. A type's own methods come before its interfaces'. When two interfaces a type implements both have the name, `x.go()` is an error, and the interface's name says which one to call:

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

## Errors

Besides mismatched and missing members (D72):

- Implementing the same interface twice for a type.
- A member that isn't in the interface.
- An interface used as a type, as in `fn f(s: Shape)`. Take a generic parameter with it as a bound instead: `fn f<T: Shape>(s: T)`.
- An impl that's never used, when neither the interface nor the type is `pub`.

## In C

Interface calls are direct calls. Generic code has a copy for each type, and each impl's functions are plain C functions named for the type and the interface, such as `Lift__Up__go`. Constants are inlined. Nothing is looked up at run time.

`dyn Shape`, for holding values of different types together, is designed but not built yet, along with associated types, generic interfaces and the operator interfaces ([spec](../CALX_SPEC.md#interfaces-and-generics-d26)).
