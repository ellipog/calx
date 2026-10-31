---
title: std.java.math
description: Math functions with Java's StrictMath results bit for bit, ported from fdlibm, for code that must compute exactly what Java code computes.
---

# `std.java.math`

Math functions that give exactly what Java's `StrictMath` gives, on every platform. That matters for code ported from Java whose results depend on the last bit, such as Minecraft's world generation, and every `nextGaussian()`, which calls `log`. Other code should use [`std.math`](math.md), which has the same functions under the same names, faster where a better algorithm exists (D97).

```calx
import std.java.math.{exp, log, pow}

fn main() {
    println("{log(1.0)} {log(2.0)} {log(10.0)}")    // out: 0.0 0.6931471805599453 2.302585092994046
    println("{exp(1.0)} {pow(7.0, 22.0)}")          // out: 2.7182818284590455 3.909821048582988E18
}
```

`exp(1.0)` is one ULP above `E` here, and `pow(7.0, 22.0)` one ULP below the double nearest 7²², because that's what `StrictMath` gives. `std.math` gives `2.718281828459045` and `3.9098210485829883E18`.

It has `PI`, `E`, `exp`, `expm1`, `log`, `log10`, `log1p`, `pow`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `hypot`, `cbrt`, `sinh`, `cosh` and `tanh`, with the same arguments and special values as `StrictMath`'s.

## How it matches

- Each function is ported from fdlibm 5.3, the library `StrictMath` is defined by, keeping its constants and its order of operations, which decide the last bit (D59, D60). fdlibm's permissive notices are kept in the source.
- Java code often calls `Math`, which HotSpot may replace with intrinsics that are off by an ULP. With those intrinsics off, `Math` and `StrictMath` agree, and Calx matches both ([spec](../CALX_SPEC.md#transcendental-functions)).
- Every function matched JDK 25's `StrictMath` on 1,043,282 arguments, in debug and release builds, on Windows and Linux: zeros, subnormals, infinities, NaN, the ends of each range, numbers next to multiples of pi/2, the arguments whose reduction by pi/2 is hardest, and hundreds of thousands of random ones. [tests/std/java_math.calx](../../tests/std/java_math.calx) keeps the hardest 138.

## Speed

The port keeps fdlibm's arithmetic but not its control flow, where a change can't alter a result: `sin`, `tan` and `atan` are exactly odd and `cos` exactly even in fdlibm, so they work on |x| and apply the sign at the end, and a case fdlibm splits off that computes what the general one does is merged into it. Nanoseconds per call on random arguments, against `StrictMath` on JDK 25:

| Function | `std.java.math` | `StrictMath` |
| --- | --- | --- |
| `sin` | 8.1 | 13.6 |
| `cos` | 8.2 | 13.3 |
| `tan` | 10.3 | 12.5 |
| `exp` | 3.7 | 5.3 |
| `log` | 3.6 | 4.4 |
| `pow` | 29.5 | 56.3 |
| `atan2` | 9.9 | 18.2 |
| `acos` | 5.8 | 7.0 |

`std.java.math` is written in Calx: [std/java/math.calx](../../std/java/math.calx).
