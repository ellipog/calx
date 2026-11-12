// Word count: 5 million words of text, about 30 MB, lowercased, split into runs of ASCII letters,
// counted in the language's hash map, and the 20 most common printed, ties by the word.
use std::collections::HashMap;

fn main() {
    let path = std::env::args().nth(1).expect("the input's path");
    let text = std::fs::read_to_string(path).expect("the input").to_ascii_lowercase();
    let mut counts: HashMap<&str, i32> = HashMap::new();
    let mut total = 0i64;
    for word in text.split(|c: char| !c.is_ascii_lowercase()).filter(|w| !w.is_empty()) {
        total += 1;
        *counts.entry(word).or_insert(0) += 1;
    }
    let mut top: Vec<(&str, i32)> = counts.iter().map(|(w, c)| (*w, *c)).collect();
    top.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(b.0)));
    println!("words {}\ndistinct {}", total, counts.len());
    for (w, c) in top.iter().take(20) {
        println!("{} {}", c, w);
    }
}
