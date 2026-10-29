// The loops of bench/strings/strings.calx, with Rust's String.
use std::hint::black_box;
use std::time::Instant;

fn letters(xs: &[i32], abc: &[char]) -> String {
    let mut out = String::new();
    for &x in xs {
        out.push(abc[(x & 3) as usize]);
    }
    out
}

fn letters_into(out: &mut String, xs: &[i32], abc: &[char]) {
    for &x in xs {
        out.push(abc[(x & 3) as usize]);
    }
}

fn wide(xs: &[i32], rs: &[char]) -> String {
    let mut out = String::new();
    for &x in xs {
        out.push(rs[(x & 3) as usize]);
    }
    out
}

fn words(xs: &[i32], ws: &[String]) -> String {
    let mut out = String::new();
    for &x in xs {
        out.push_str(&ws[(x & 3) as usize]);
    }
    out
}

fn escape(out: &mut String, text: &str) {
    for r in text.chars() {
        match r {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            _ => out.push(r),
        }
    }
}

fn main() {
    let mut xs = Vec::new();
    let mut s: u64 = 1;
    for _ in 0..1000000 {
        s = s.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        xs.push((s >> 32) as i32);
    }
    let abc = ['a', 'b', 'c', 'd'];
    let rs = ['a', 'é', '€', '😀'];
    let ws: Vec<String> = ["air", "stone", "dirt", "oak_log"].iter().map(|w| w.to_string()).collect();
    let marks = ['T', 'h', 'e', ' ', 'q', 'u', 'i', 'c', 'k', ' ', 'f', 'o', 'x', '"', '\n', 'é'];
    let mut text = String::new();
    for &x in &xs {
        text.push(marks[(x & 15) as usize]);
    }
    let mut best = [1000000.0f64; 5];
    let letters_far = black_box(letters_into as fn(&mut String, &[i32], &[char]));
    let escape_far = black_box(escape as fn(&mut String, &str));
    let mut a = String::new();
    let mut e = String::new();
    let mut total = 0usize;
    for _ in 0..50 {
        let t0 = Instant::now();
        let na = letters(&xs, &abc);
        let t1 = Instant::now();
        let nb = wide(&xs, &rs);
        let t2 = Instant::now();
        let nc = words(&xs, &ws);
        let t3 = Instant::now();
        a.clear();
        letters_far(&mut a, &xs, &abc);
        let t4 = Instant::now();
        e.clear();
        escape_far(&mut e, &text);
        let t5 = Instant::now();
        let ms = |a: Instant, b: Instant| (b - a).as_secs_f64() * 1000.0;
        best[0] = best[0].min(ms(t0, t1));
        best[1] = best[1].min(ms(t1, t2));
        best[2] = best[2].min(ms(t2, t3));
        best[3] = best[3].min(ms(t3, t4));
        best[4] = best[4].min(ms(t4, t5));
        total += na.len() + nb.len() + nc.len() + a.len() + e.len();
    }
    println!("new: letters {} ms, wide {} ms, words {} ms", best[0], best[1], best[2]);
    println!("given, not inlined: letters {} ms, escape {} ms", best[3], best[4]);
    println!("{}", total);
}
