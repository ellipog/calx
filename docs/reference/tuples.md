---
title: Tuples
description: Fixed groups of values by position - literals, .0, destructuring, and tuples as parameters and results.
---

# Tuples

A tuple groups two or more values by position. Its type lists theirs: `(i32, str)`.

```calx
fn min_max(xs: [i32; 4]) -> (i32, i32) {
    var lo = xs[0]
    var hi = xs[0]
    for x in xs {
        lo = lo.min(x)
        hi = hi.max(x)
    }
    return (lo, hi)
}

fn main() {
    let (lo, hi) = min_max([4, -2, 9, 3])
    println("{lo} {hi}")                 // out: -2 9

    let t = (1, 2.5, true)
    println("{t.0} {t.1} {t.2}")         // out: 1 2.5 true

    var p = (0, 0)
    p.1 = 5
    println("{p.0} {p.1}")               // out: 0 5
}
```

- `(a, b, ...)` builds one, running its parts left to right. A tuple has at least two parts: `(x)` is just `x` in parentheses, and `()` is the [unit value](types.md#the-unit-type).
- `t.0`, `t.1` and so on read a part. On a `var` they can be assigned.
- Tuples are values, copied when assigned, like structs.
- Tuples compare with `==` part by part, and print as `(1, a, true)` ([comparing and printing](comparing.md)). `match` sees inside them.

## Destructuring

A tuple pattern takes a tuple apart. It works in `let`, `var`, `for`, `match` and `if let` ([patterns](patterns.md)), and `_` skips a part:

```calx
fn main() {
    let ((a, _), c) = ((1, 2), 3)
    var (x, y) = (1, 2)                  // `var` makes each name a variable
    x += a
    y *= c
    println("{x} {y}")                   // out: 2 6

    for (i, ch) in [(1, 'a'), (2, 'b')] {
        print("{i}{ch};")
    }
    println()                            // out: 1a;2b;
}
```

A tuple in C is a struct with one field per part, so it costs what the separate values would.
