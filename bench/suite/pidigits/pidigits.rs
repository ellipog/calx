// pidigits: the Benchmarks Game's digits of π from the unbounded spigot, ten to a line, with
// integers of any size. Each language uses its own big integers: std.bigint in Calx, num-bigint
// here, math/big in Go and Python's int. C has none in its standard library.
use num_bigint::BigInt;
use num_traits::{One, ToPrimitive, Zero};
use std::io::{self, BufWriter, Write};

fn main() -> io::Result<()> {
    let n: u32 = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(27);
    let stdout = io::stdout();
    let mut out = BufWriter::new(stdout.lock());
    let mut numer = BigInt::one();
    let mut accum = BigInt::zero();
    let mut denom = BigInt::one();
    let mut line = String::new();
    let (mut i, mut k) = (0u32, 0u32);
    while i < n {
        k += 1;
        let k2 = k * 2 + 1;
        accum = (accum + &numer * 2u32) * k2;
        denom *= k2;
        numer *= k;
        if numer > accum {
            continue;
        }
        let d = (&numer * 3u32 + &accum) / &denom;
        if d != (&numer * 4u32 + &accum) / &denom {
            continue;
        }
        let digit = d.to_u32().unwrap();
        line.push(char::from_digit(digit, 10).unwrap());
        i += 1;
        if i % 10 == 0 {
            writeln!(out, "{}\t:{}", line, i)?;
            line.clear();
        }
        accum = (accum - &denom * digit) * 10u32;
        numer *= 10u32;
    }
    if !line.is_empty() {
        writeln!(out, "{:<10}\t:{}", line, n)?;
    }
    out.flush()
}
