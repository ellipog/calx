// bench/text/text.calx with Rust's str methods, written as Rust code is usually written.
use std::time::Instant;

fn main() {
    let mut text = String::new();
    for i in 0..200000u64 {
        text.push_str(&format!("  {}, name {} , {}, some more text here \n", i, i, i * 7));
    }
    let (mut loops, mut join, mut replace) = (u128::MAX, u128::MAX, u128::MAX);
    let mut total = 0usize;
    for _ in 0..7 {
        let t0 = Instant::now();
        let mut fields = 0usize;
        for line in text.lines() {
            for c in line.split(',') {
                fields += c.trim().len();
            }
        }
        let t1 = Instant::now();
        let joined = text.lines().collect::<Vec<_>>().join("|");
        let t2 = Instant::now();
        let replaced = joined.replace("name", "NAME");
        let t3 = Instant::now();
        total += fields + replaced.len();
        loops = loops.min((t1 - t0).as_micros());
        join = join.min((t2 - t1).as_micros());
        replace = replace.min((t3 - t2).as_micros());
    }
    println!("rust: fields {} us, lines joined {} us, replace {} us, total {}", loops, join, replace, total);
}
