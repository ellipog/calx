---
title: std.math
description: Math functions with Java's StrictMath results bit for bit, ported from fdlibm - log so far.
---

# `std.math`

Math functions that give exactly what Java's `StrictMath` gives, on every platform. Calcite needs the last bit to match, since vanilla's worldgen and random numbers go through them: every `nextGaussian()` calls `log`.

```calx
import std.math.{log}

fn main() {
    println("{log(1.0)} {log(2.0)} {log(10.0)}")    // out: 0.0 0.6931471805599453 2.302585092994046
    println("{log(0.0)} {log(-1.0)}")                // out: -Infinity NaN
}
```

| Function | Gives |
| --- | --- |
| `log(x: f64) -> f64` | The natural logarithm, as `StrictMath.log`. `log(0.0)` and `log(-0.0)` are `-Infinity`, `log(Infinity)` is `Infinity`, and a negative number or NaN gives NaN |

- Each function is ported from fdlibm 5.3, the library `StrictMath` is defined by, keeping its constants and its order of operations, which decide the last bit (D59). fdlibm's permissive notice is kept in the source.
- Vanilla calls `Math`, which HotSpot may replace with intrinsics that are off by an ULP. The rig's reference runs vanilla with those intrinsics off, so `Math` and `StrictMath` agree there, and Calx matches both ([spec](../CALX_SPEC.md#transcendental-functions)).
- `log` matched `StrictMath.log` on 4,518 arguments: zeros, subnormals, infinities, NaN, the largest double, numbers just either side of 1, and thousands of random ones.
- `sqrt`, `floor` and `ceil` are methods on the float types, since IEEE 754 makes them exact everywhere ([floats](../reference/floats.md)).

The other functions, `sin`, `cos`, `pow`, `exp` and the rest, are designed in the [spec](../CALX_SPEC.md#transcendental-functions) and come as Calcite needs them.

`std.math` is written in Calx: [std/math.calx](../../std/math.calx).
