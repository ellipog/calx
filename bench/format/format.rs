// bench/format/format.calx with Rust's `write!` into a `String`.
use std::fmt::Write;
use std::time::Instant;

fn main() {
    let mut x: u64 = 88172645463325252;
    let mut ints: Vec<i64> = Vec::new();
    let mut doubles: Vec<f64> = Vec::new();
    for k in 0..1000000 {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        ints.push((x >> 44) as i64);
        if k < 200000 {
            doubles.push(1.0 + (x >> 11) as f64 * 999.0 / 9007199254740992.0);
        }
    }
    let (mut fixed, mut padded, mut plain, mut shortest) = (u128::MAX, u128::MAX, u128::MAX, u128::MAX);
    let mut total = 0usize;
    for _ in 0..7 {
        let t0 = Instant::now();
        let mut a = String::new();
        for v in &doubles {
            write!(a, "{:.3},", v).unwrap();
        }
        let t1 = Instant::now();
        let mut b = String::new();
        for i in &ints[..200000] {
            write!(b, "{:>8},", i).unwrap();
        }
        let t2 = Instant::now();
        let mut c = String::new();
        for n in &ints {
            write!(c, "{},", n).unwrap();
        }
        let t3 = Instant::now();
        let mut d = String::new();
        for v in &doubles {
            write!(d, "{},", v).unwrap();
        }
        let t4 = Instant::now();
        total += a.len() + b.len() + c.len() + d.len();
        fixed = fixed.min((t1 - t0).as_micros());
        padded = padded.min((t2 - t1).as_micros());
        plain = plain.min((t3 - t2).as_micros());
        shortest = shortest.min((t4 - t3).as_micros());
    }
    println!("rust: fixed {} us, padded {} us, integers {} us, shortest {} us, total {}", fixed, padded, plain, shortest, total);
}
