// Building and taking apart text: a million CSV-like lines built onto one string with formatting,
// then each line split at its commas, a field uppercased and three joined with " | " onto a second
// string, the lines that name one user counted, and a word replaced through the whole result.
use std::fmt::Write;

fn main() {
    let n: i64 = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(1_000_000);
    let words = ["alpha", "beta", "gamma", "delta", "epsilon"];
    let mut built = String::new();
    for i in 0..n {
        writeln!(built, "{},user{},{}.{:02},{}", i, i % 1000, i * 7 % 10007, i % 100, words[(i % 5) as usize]).unwrap();
    }
    let mut out = String::new();
    let mut matches = 0;
    for line in built.lines() {
        let fields: Vec<&str> = line.split(',').collect();
        out.push_str(&fields[3].to_uppercase());
        out.push_str(" | ");
        out.push_str(fields[1]);
        out.push_str(" | ");
        out.push_str(fields[2]);
        out.push('\n');
        if fields[1] == "user42" {
            matches += 1;
        }
    }
    let replaced = out.replace("user", "u");
    println!("built {}\nout {}\nmatches {}\nreplaced {}", built.len(), out.len(), matches, replaced.len());
}
