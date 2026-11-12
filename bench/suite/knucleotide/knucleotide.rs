// k-nucleotide: the Benchmarks Game's hash-table test. The third sequence of fasta's output is read,
// every k-mer of it counted for each k, and the 1- and 2-mers' frequencies and five longer k-mers'
// counts printed. Each k-mer is the key, two bits a base in a 64-bit integer, of the language's
// own hash map: here std's HashMap, with its default hasher.
use std::collections::HashMap;

fn code(b: u8) -> u64 {
    match b {
        b'A' => 0,
        b'C' => 1,
        b'G' => 2,
        _ => 3,
    }
}

fn count(seq: &[u8], k: usize) -> HashMap<u64, i32> {
    let mut counts = HashMap::new();
    let mask = (1u64 << (2 * k)) - 1;
    let mut key = 0u64;
    for &b in &seq[..k - 1] {
        key = (key << 2) | code(b);
    }
    for &b in &seq[k - 1..] {
        key = ((key << 2) | code(b)) & mask;
        *counts.entry(key).or_insert(0) += 1;
    }
    counts
}

fn decode(key: u64, k: usize) -> String {
    (0..k).map(|i| b"ACGT"[((key >> (2 * (k - 1 - i))) & 3) as usize] as char).collect()
}

fn frequencies(seq: &[u8], k: usize) {
    let counts = count(seq, k);
    let mut entries: Vec<(u64, i32)> = counts.into_iter().collect();
    entries.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    let total = (seq.len() - k + 1) as f64;
    for (key, n) in entries {
        println!("{} {:.3}", decode(key, k), 100.0 * n as f64 / total);
    }
    println!();
}

fn occurrences(seq: &[u8], kmer: &str) {
    let counts = count(seq, kmer.len());
    let key = kmer.bytes().fold(0u64, |key, b| (key << 2) | code(b));
    println!("{}\t{}", counts.get(&key).copied().unwrap_or(0), kmer);
}

fn main() {
    let path = std::env::args().nth(1).expect("the input's path");
    let text = std::fs::read_to_string(path).expect("the input");
    let mut seq = Vec::new();
    let mut inside = false;
    for line in text.lines() {
        if line.starts_with('>') {
            if inside {
                break;
            }
            inside = line.starts_with(">THREE");
        } else if inside {
            seq.extend(line.bytes().map(|b| b.to_ascii_uppercase()));
        }
    }
    frequencies(&seq, 1);
    frequencies(&seq, 2);
    for kmer in ["GGT", "GGTA", "GGTATT", "GGTATTTTAATT", "GGTATTTTAATTTATAGT"] {
        occurrences(&seq, kmer);
    }
}
