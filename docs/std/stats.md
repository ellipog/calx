---
title: std.stats
description: Means, medians, quantiles, modes, variances, standard deviations, correlation and linear regression, giving what Python's statistics module gives to the last bit.
---

# `std.stats`

Statistics of lists of numbers, as Python's `statistics` module gives them, and with the same results: each function takes the steps Python 3.14's does, so a script ported from Python prints the same digits.

```calx
import std.stats

fn main() {
    let times: List<f64> = [2.5, 3.25, 5.5, 11.25, 11.75]
    println("{stats.mean(times)!} {stats.median(times)!} {stats.pstdev(times)!}")  // out: 6.85 5.5 3.926194085880116
    println("{stats.quantiles(times)!}")                      // out: [2.875, 5.5, 11.5]
    let x: List<f64> = [1.0, 2.0, 3.0, 4.0]
    let y: List<f64> = [2.1, 3.9, 6.2, 7.8]
    let (slope, intercept) = stats.linear_regression(x, y)!
    println("{slope:.2} {intercept:.2} {stats.correlation(x, y)!:.4}")  // out: 1.94 0.15 0.9978
}
```

| Function | Gives |
| --- | --- |
| `fsum(xs)` | The sum, correctly rounded: the `f64` nearest the exact sum, whatever the order, as Python's `math.fsum` |
| `mean(xs)` | The arithmetic mean |
| `geometric_mean(xs)` | The nth root of the product, from logarithms |
| `harmonic_mean(xs)` | The count over the sum of reciprocals |
| `median(xs)` | The middle value, or the mean of the two in the middle |
| `quantiles(xs, n: 4, inclusive: false)` | The `n - 1` cut points that split the sorted values into `n` groups: the quartiles by default, `quantiles(xs, 100)` for percentiles. `inclusive: true` takes the values as a whole population, Python's `method="inclusive"` |
| `mode(xs)` | The most common value, the first to appear when several are as common. It takes any list whose elements can be a map's key, such as text or integers |
| `variance(xs)`, `stdev(xs)` | The variance and the standard deviation of a sample, dividing by one less than the count |
| `pvariance(xs)`, `pstdev(xs)` | The same for a whole population, dividing by the count |
| `covariance(xs, ys)` | The covariance of two samples |
| `correlation(xs, ys)` | Pearson's correlation coefficient, from -1 to 1 |
| `linear_regression(xs, ys)` | The slope and the intercept of the least-squares line, as a tuple |

- Each gives an optional: `none` where Python raises `StatisticsError`. That's no values for a mean, a median, a mode or a population's variance, one value or none for a sample's, fewer than two pairs for the functions of two lists, a list whose values are all the same for `correlation` and `linear_regression`, and a negative value for the geometric and harmonic means.
- The functions of two lists panic when the lists' lengths differ, as giving them different lengths is a mistake in the program.
- `median` and `quantiles` sort a copy, with NaNs last, as [`total_cmp`](../reference/floats.md#methods) orders them.

## Exactly as Python

Python works out the mean and the variances with fractions, exactly, and rounds once at the end. Adding up floats and dividing would round at every step, so `pvariance([2.5, 3.25, 5.5, 11.25, 11.75])` would be `15.415000000000001`, not Python's `15.415`.

`std.stats` gets Python's results without its fractions, which are slow:

- A sum is kept as Shewchuk's partial sums, a few floats that add up to it exactly, as `math.fsum` keeps them.
- A square is added as two floats, the rounded square and what rounding left out, which `mul_add` (a fused multiply-add) gives exactly.
- At the end the partial sums become one [`BigInt`](bigint.md), and the quotient is rounded once with `div_f64`.
- The standard deviations round the square root of the exact variance correctly, as Python's `_float_sqrt_of_frac` does.

So each value costs a few float operations, as a plain loop would, and the exact arithmetic happens once.

`covariance`, `correlation` and `linear_regression` take Python's float steps, with its correctly rounded `sumprod`.

Checked against Python 3.14 on 400 random data sets, of 2 to 1,000 values each, with means near zero and near a billion, values from 2^-60 to 2^60, and repeated values. Every function gave the same bits, except for one geometric mean in the last bit: it takes logarithms and an exponential, and `std.math`'s differ from the C library's that Python calls in the last bit now and then.

The other difference is deliberate. With an infinity or a NaN among the values, the variances and standard deviations are NaN, where Python gives an infinity for a variance and raises for a standard deviation.
