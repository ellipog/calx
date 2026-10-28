---
title: Structs
description: Declaring structs, field defaults, building values, and methods in impl blocks.
---

# Structs

```calx
struct Vec2 {
    x: f64
    y: f64 = 0.0
}

impl Vec2 {
    fn zero() -> Vec2 {
        return Vec2 { x: 0.0 }
    }

    fn length(self) -> f64 {
        return (self.x * self.x + self.y * self.y).sqrt()
    }

    fn scale(v: mut Vec2, by: f64) {
        v.x *= by
        v.y *= by
    }
}

fn main() {
    var v = Vec2 { x: 3.0, y: 4.0 }
    v.scale(2.0)
    let origin = Vec2.zero()
    println("{v.length()} {origin.x}")    // out: 10.0 0.0
}
```

## Declaring

`struct Name { field: Type ... }` declares a struct, with one field per line (or separated by commas). A field can have a default, `y: f64 = 0.0`, which must be a constant. A struct can't hold itself by value.

## Building a value

- `Name { field: value, ... }` gives every field without a default. Fields can be written in any order, and their values run in the order written.
- `Name { x, y }` takes the variables named `x` and `y`.
- `Name()` builds a value with every field at its default, and needs every field to have one. Since every default is a constant, so is `Name()`, which can be another struct's default: `look: Look = Look()`.

```calx
struct Settings {
    distance: i32 = 10
    hardcore: bool = false
}

fn main() {
    let a = Settings()
    let b = Settings { hardcore: true }
    println("{a.distance} {b.hardcore}")    // out: 10 true
}
```

## Fields

`value.field` reads a field. On a `var`, or a `mut` parameter, it can be assigned. A struct of `Copy` fields is copied when assigned, so changing a copy doesn't change the original. A struct with a field that owns memory, such as a `String`, moves instead ([ownership](ownership.md)).

A field is private to its module unless it's marked `pub`, as in `pub x: f64` ([modules](modules.md#pub)). The same goes for a function in an `impl` block.

Two structs of the same type compare with `==` field by field, and `{p}` in a string shows one as its literal, `Point { x: 1, y: 2 }` ([comparing and printing](comparing.md)).

## Methods

An `impl Name { }` block adds functions to a struct declared in the same module.

- A function whose first parameter is the struct, in any mode, is a method, called as `value.method(...)`. `self` alone is a read-only `self: Name`. A method that changes its value takes `mut self`, and is called on a variable. One that keeps or takes its value apart takes `own self`, which moves the value in.
- Any other function in the block is called as `Name.function(...)`.
- Inside an impl block, `Self` means the struct.
- A struct can't have a field and a method with the same name.
