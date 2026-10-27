---
title: Variables
description: let and var, constants, and the rules against unused, unchanged and shadowed names.
---

# Variables

```calx
const LIMIT: i32 = 100

fn main() {
    let speed = 5            // let: never changes
    var distance = 0         // var: can be assigned again
    while distance < LIMIT {
        distance += speed
    }
    println("{distance}")    // out: 100
}
```

- `let name = value` declares a variable that never changes. `var name = value` declares one that can be assigned again, or have its fields and elements changed.
- A type can be given: `let x: u8 = 200`. Otherwise it's the value's type.
- Every variable has a value from the start. Calx has no uninitialized variables, so `let x: i32` alone is an error. A value that depends on a condition comes from an `if` used as a value: `let x = if c { 1 } else { 2 }`.
- A variable is visible from its declaration to the end of its block.

## Constants

`const NAME: T = value` declares a constant at the top level of a module. The type must be given, and the value must be known while compiling: literals, other constants and operators on them. Constants can be integers, floats, `bool`, `char` or `rune`, or arrays of them, such as a table of coefficients:

```calx
const STEP: i32 = 4
const WEIGHTS: [f64; 3] = [0.25, 0.5, 0.25]
const STEPS: [i32; 3] = [STEP, STEP * 2, STEP * 3]
const GRID: [[u8; 2]; 2] = [[1, 2], [3, 4]]

fn main() {
    var sum = 0.0
    for i in 0usize..3 {
        sum += WEIGHTS[i] * f64(STEPS[i])
    }
    println("{sum} {GRID[1][0]}")    // out: 8.0 3
}
```

An integer constant is written into the code wherever it's used, so it costs nothing. A float or an array is a `static const` in C, in each file that uses it. Reading an element of an array constant is bounds checked, at compile time when the index is a constant.

## Assignment

`place = value` assigns to a variable, a field of one or an element of one. `place op= value` is `place = place op value`, for every arithmetic, bitwise and shift operator. Java's order holds: the place's parts, such as an index, are worked out first, then the value.

`_ = value` evaluates `value` and discards it, for a result that isn't needed.

## Unused names

The compiler rejects names that do nothing (D63). Each of these is an error:

- A variable or parameter that's never read. A name starting with `_` says it's unused on purpose.
- A `var` that's never assigned again or changed. Declare it with `let`.
- A `mut` parameter that's never changed. Take it without `mut`.
- A function, struct or constant that's never used, unless it's `pub` or it's `main`.
- An [import](modules.md#importing) that's never used.

Assigning to a variable isn't reading it, so a counter that's only ever increased is unused.

```calx
fn main() {
    let unused = 5        // error: `unused` is never used
    var fixed = 1         // error: `fixed` is declared with `var` but never changed; declare it with `let`
    var count = 0         // error: `count` is never used
    count += 1
    let _skip = 3
    println("{fixed}")
}
```

## No shadowing

A new variable can't take a name that's already visible: another variable, a parameter, a function, a struct, a constant or a built-in name. In ported code, a reused name is usually a bug.

```calx
fn main() {
    let x = 1
    if x > 0 {
        let x = 2     // error: `x` is already defined; Calx doesn't allow shadowing
        println("{x}")
    }
}
```

Two variables in separate blocks can have the same name, since neither is visible from the other.
