---
title: Interfaces
description: Interfaces with methods, constants and defaults; impls for structs, enums and primitives; generic interfaces such as Into<T>; associated types; dyn values of any type with an interface; and naming the interface.
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
- `const name: Type` is a constant each impl gives. It's a constant wherever it's used, so `T.sides` can be a field's default. With a value, `const width: i32 = 4`, it's a default, which an impl that leaves it out takes, worked out for its type, so `Self.other` in it is the impl's.
- A function can be generic, `fn put<T: Display>(mut self, x: T)`. Each impl's is generic the same way, with no more bounds than the interface gives, since a call through the interface knows only those. A call works out `T`, as a generic function's does, and calls the impl's copy for it.
- A function with a body is a default, as `size` is above. Impls get it without writing it, and can replace it.
- `Self` is the type implementing the interface: `fn bigger(self, other: Self) -> bool`, `fn unit() -> Self`.
- `type Item` is a type each impl gives, which the interface's functions can name as `Self.Item` ([associated types](#associated-types)).

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
- A literal argument such as `2` or `1.5` takes the type of the one impl it can be: if `Meters` had `Scale<i64>` and `Scale<Meters>`, `m.scale(2)` would take `Scale<i64>`. When two impls could take it, it's an `i32` or an `f64`, as a literal is anywhere else.
- A generic interface is always written with its arguments: a bound `T: Into` is an error that asks for them.
- An impl for a generic type can use its parameters in the arguments: `impl<A: Copy> Into<List<A>> for Pair<A>`.
- Defaults can use the interface's parameters, as `twice` does.
- Every parameter must be used by a member, or no use could tell what it is.
- [`From<T>`](errors.md#converting-errors) is a built-in generic interface, which `try` converts errors with.
- [`Iterator<T>`](control-flow.md#iterators) is another, with `fn next(mut self) -> T?`, which a `for` loop calls until it gives `none`.
- So are the [operators'](#operators): `Add<T>`, `Sub<T>`, `Mul<T>`, `Div<T>` and `Rem<T>`.

## Associated types

`type Item` in an interface is a type each impl gives, `type Item = i32`, where a generic interface would need a parameter that every use repeats. `C.Item` names it for any type `C` that implements the interface, and `Self.Item` names it in the interface and its impls (CLX-72):

```calx
interface Container {
    type Item: Display

    fn get(self, i: usize) -> Self.Item?

    fn len(self) -> usize
}

struct Bag {
    items: List<i32>,
}

impl Container for Bag {
    type Item = i32

    fn get(self, i: usize) -> i32? {
        return if i < self.items.len() { some(self.items[i]) } else { none }
    }

    fn len(self) -> usize {
        return self.items.len()
    }
}

// Generic code knows only the bounds the interface gives `Item`: it can be shown.
fn last<C: Container>(c: C) -> C.Item? {
    return if c.len() == 0 { none } else { c.get(c.len() - 1) }
}

fn main() {
    let b = Bag { items: [3, 1, 4] }
    let l: i32 = last(b)!
    let n: Bag.Item = 7
    println("{l} {n}")    // out: 4 7
}
```

- An impl gives each of the interface's types, after `type` and `=`. Its functions can write the type itself, `i32`, or `Self.Item`, which is the same.
- `type Item: Display + Copy` gives the type bounds, as a generic parameter's are. Each impl's type must have them, and generic code can use only them: in `last`, a `C.Item` can be shown, and nothing more.
- For a real type, `Bag.Item` is the type its impl gives, here `i32`. For a parameter, `C.Item` is a type of its own until a call says what `C` is, so `last(b)` gives back an `i32?`.
- A field can be one, `held: C.Item` in a `struct Holder<C: Container>`, and an impl for a generic type can give its parameter, `type Item = T`.
- A generic interface can have them too, `interface Codec<In> { type Out }`, and `C.Out` is for the bound `C: Codec<u8>`.
- A method that mentions `Self.Item` mentions `Self`, so it isn't in a [`dyn`](#holding-any-type-dyn)'s table.

## Holding any type: `dyn`

A generic parameter is one type for each copy of the code. `dyn Shape` is a value of any type that implements `Shape`, so a list can hold a circle and a square together, a field can hold either, and a function can return either (CLX-72):

```calx
interface Shape {
    fn area(self) -> f64

    fn name(self) -> String {
        return "shape"
    }

    fn grow(mut self, by: f64)
}

struct Circle {
    r: f64,
}

struct Square {
    side: f64,
}

impl Shape for Circle {
    fn area(self) -> f64 {
        return 3.0 * self.r * self.r
    }

    fn grow(mut self, by: f64) {
        self.r += by
    }
}

impl Shape for Square {
    fn area(self) -> f64 {
        return self.side * self.side
    }

    fn name(self) -> String {
        return "square"
    }

    fn grow(mut self, by: f64) {
        self.side += by
    }
}

fn describe(s: dyn Shape) -> String {
    return "{s.name()} {s.area()}"
}

fn pick(big: bool) -> dyn Shape {
    if big {
        return Square { side: 10.0 }
    }
    return Circle { r: 1.0 }
}

fn main() {
    let c = Circle { r: 1.0 }
    println(describe(c))    // out: shape 3.0
    var shapes: List<dyn Shape> = [c, Square { side: 2.0 }]
    shapes.push(pick(true))
    for s in mut shapes {
        s.grow(1.0)
    }
    for s in shapes {
        println(describe(s))
    }
    // out: shape 12.0
    // out: square 9.0
    // out: square 121.0
}
```

- A value becomes a `dyn Shape` wherever one is wanted, when its type implements `Shape`: in a variable, a list, a field, a return, or an argument. There's nothing to write.
- A `dyn` owns its value, on the heap, so making one in a variable, a list or a field moves the value there. An argument for a parameter that only looks at it, as `describe`'s, is a view of the value where it is: `c` is still `c` after `describe(c)`, and nothing is allocated.
- It calls the methods that take `self` or `mut self` and mention `Self` nowhere else, the interface's defaults included. A function without `self`, one that takes `own self`, one that takes or gives back a `Self`, a generic one, and a constant aren't in its table, since which type it holds isn't known, and calling one says why.
- `.copy()` copies the value it holds, so every type that becomes a `dyn` must be one that can be copied: a type holding a `Thread` or a `Library` can't.
- It can't be compared, shown in a string, hashed or ordered, since those depend on the type it holds; give the interface a method for what's needed, such as `fn label(self) -> String`.
- A generic interface takes its arguments: `dyn Into<i64>`.
- A `mut dyn Shape` parameter can be given a value of another type, so its argument must be a `dyn Shape` variable already, not a `Circle`.

## Operators

`+`, `-`, `*`, `/` and `%` on a struct or an enum call its impl of `Add<T>`, `Sub<T>`, `Mul<T>`, `Div<T>` or `Rem<T>`, where `T` is the type on the right, and a prefix `-` calls `Neg`. They're built in, as `Equal` is (CLX-143):

```calx
struct Vec2 {
    x: f64
    y: f64
}

impl Add<Vec2> for Vec2 {
    fn add(self, other: Vec2) -> Vec2 {
        return Vec2 { x: self.x + other.x, y: self.y + other.y }
    }
}

impl Mul<f64> for Vec2 {
    fn mul(self, other: f64) -> Vec2 {
        return Vec2 { x: self.x * other, y: self.y * other }
    }
}

impl Neg for Vec2 {
    fn neg(self) -> Vec2 {
        return Vec2 { x: -self.x, y: -self.y }
    }
}

// Numbers have the operators with themselves, so this sums `i32`s and `Vec2`s alike.
fn sum<T: Add<T> + Copy>(xs: List<T>, zero: T) -> T {
    var acc = zero
    for x in xs {
        acc += x
    }
    return acc
}

fn main() {
    let a = Vec2 { x: 1.0, y: 2.0 }
    var b = a * 3.0 + -a
    b += a
    println("{b.x} {b.y}")                                  // out: 3.0 6.0
    let vs: List<Vec2> = [a, b]
    let ns: List<i32> = [1, 2, 3]
    println("{sum(vs, Vec2 { x: 0.0, y: 0.0 }).x} {sum(ns, 0)}")  // out: 4.0 6
}
```

| Interface | Function | Written |
| --- | --- | --- |
| `Add<T>` | `fn add(self, other: T) -> Self` | `a + b` |
| `Sub<T>` | `fn sub(self, other: T) -> Self` | `a - b` |
| `Mul<T>` | `fn mul(self, other: T) -> Self` | `a * b` |
| `Div<T>` | `fn div(self, other: T) -> Self` | `a / b` |
| `Rem<T>` | `fn rem(self, other: T) -> Self` | `a % b` |
| `Neg` | `fn neg(self) -> Self` | `-a` |

- Both sides are views, as a method's receiver and arguments are, so neither is moved, and the result is a new value. A type that owns memory, such as a big integer, keeps its operands: `a * b + a` reads `a` twice and makes two new values, and drops the one in between.
- `a += b` is `a = a + b`, and the same for the others. The place is read and then set, so it can't hold a call, which would run twice: `xs[next()] += v` is an error, and `xs[i] += v` isn't.
- An operator's function can be [`@track_caller`](printing.md#panics-that-name-the-caller), so its panics name the line that used the operator, as dividing a `BigInt` by zero does (CLX-145).
- A type can implement an operator for more than one type on the right, such as `Add<Money>` and `Add<i64>`, and the right side chooses, as for any [generic interface](#generic-interfaces). A literal there takes the type of the one impl it can be, so `m + 50` takes `Add<i64>`.
- The value on the left is the one whose impl is called, and a number can't have one, so `2.0 * v` is an error that says to write `v * 2.0`.
- In generic code, a bound such as `T: Add<T>` lets the body use `+` on `T`. Every number satisfies `Add`, `Sub`, `Mul`, `Div` and `Rem` with its own type, and signed integers and floats satisfy `Neg`, so an instance for numbers uses their own operators, which wrap, as they do anywhere.
- `add` and the others are what the operators call, as `eq` is what `==` calls, so `a.add(b)` is an error that says to write `a + b`. A type's own `add`, in its own impl, is a method like any other.
- Comparisons come from [`Equal` and `Order`](comparing.md), which a type usually gets without writing them. Bitwise operators, shifts and indexing don't call interfaces.

## Errors

Besides mismatched and missing members (D72):

- Implementing the same interface twice for a type, or a generic interface twice with the same arguments.
- A member that isn't in the interface.
- An interface used as a type, as in `fn f(s: Shape)`. Take a generic parameter with it as a bound instead, `fn f<T: Shape>(s: T)`, or a [`dyn Shape`](#holding-any-type-dyn) to take any type with it.
- A call through a `dyn` of a function that isn't in its table, and a value for a `dyn` whose type doesn't implement the interface.
- An impl without one of the interface's types, a type the interface doesn't have, or one without the bounds the interface gives it; and `C.Item` for a `C` whose bounds have no `Item`.
- An impl that's never used, when neither the interface nor the type is `pub`.

## In C

Interface calls are direct calls. Generic code has a copy for each type, and each impl's functions are plain C functions named for the type and the interface, such as `Lift__Up__go`, and for a generic interface's arguments too, such as `Meters__Into_i64__into`. Constants are inlined. Nothing is looked up at run time, even when a call chooses between impls: the checker chooses. An operator is such a call too, `a + b` being `Vec2__Add_Vec2__add(&a, &b)`, and in a generic function's copy for a number, it's the number's own `+`.

A `dyn Shape` is two addresses: its value's, and a table of its type's functions, which is a constant. The table has the type's drop and copy, then each method in the order the interface declares them, and a call is one load and an indirect call: `s.area()` is `s->vt->m0(s->data)`. A view for an argument that's only looked at is a `dyn` made in place, with the value's own address. Each file that makes a `dyn` of a type has that type's table, so a program has one for each type and interface it uses together.

An associated type is only a name for a type: in each generic function's copy, `C.Item` is the type the impl gives, so it costs nothing.
