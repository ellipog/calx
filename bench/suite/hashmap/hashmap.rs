// Hash maps: 2 million integer keys inserted, 4 million looked up, half of them missing, every
// other key removed and all looked up again, and the values summed; then 500,000 string keys
// inserted and a million looked up. Each language uses its standard map with its default hash:
// here std's HashMap with SipHash-1-3, which resists flooding, where many Rust programs swap in a
// faster hasher.
use std::collections::HashMap;

fn main() {
    let dir = std::env::args().nth(1).expect("the input folder");
    let ib = std::fs::read(format!("{}/ints.bin", dir)).unwrap();
    let ints: Vec<i64> = ib.chunks_exact(8).map(|c| i64::from_le_bytes(c.try_into().unwrap())).collect();
    let n: usize = std::env::args().nth(2).and_then(|s| s.parse().ok()).unwrap_or(2_000_000);
    let mut m: HashMap<i64, i64> = HashMap::new();
    for (i, &k) in ints[..n].iter().enumerate() {
        m.insert(k, i as i64);
    }
    let (mut hits, mut sum) = (0i64, 0i64);
    for k in &ints[..2 * n] {
        if let Some(v) = m.get(k) {
            hits += 1;
            sum += v;
        }
    }
    println!("inserted {}\nfound {} {}", m.len(), hits, sum);
    for (i, k) in ints[..n].iter().enumerate() {
        if i % 2 == 0 {
            m.remove(k);
        }
    }
    let hits = ints[..n].iter().filter(|k| m.contains_key(k)).count();
    let total: i64 = m.values().sum();
    println!("left {} {} {}", m.len(), hits, total);

    let text = std::fs::read_to_string(format!("{}/strings.txt", dir)).unwrap();
    let strings: Vec<&str> = text.lines().collect();
    let mut s: HashMap<String, i64> = HashMap::new();
    for (i, w) in strings[..strings.len() / 2].iter().enumerate() {
        s.insert(w.to_string(), i as i64);
    }
    let (mut hits, mut sum) = (0i64, 0i64);
    for w in &strings {
        if let Some(v) = s.get(*w) {
            hits += 1;
            sum += v;
        }
    }
    println!("strings {} {} {}", s.len(), hits, sum);
}
