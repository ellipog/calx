// regex-redux: the Benchmarks Game's regular-expression test. fasta's output has its headers and
// line breaks taken out by one pattern, nine variants of a motif counted, and five patterns
// replaced in turn, with the lengths printed. Each language uses its standard regex library, or
// Rust's regex crate; C has none.
use regex::{NoExpand, Regex};

const VARIANTS: [&str; 9] = [
    "agggtaaa|tttaccct",
    "[cgt]gggtaaa|tttaccc[acg]",
    "a[act]ggtaaa|tttacc[agt]t",
    "ag[act]gtaaa|tttac[agt]ct",
    "agg[act]taaa|ttta[agt]cct",
    "aggg[acg]aaa|ttt[cgt]ccct",
    "agggt[cgt]aa|tt[acg]accct",
    "agggta[cgt]a|t[acg]taccct",
    "agggtaa[cgt]|[acg]ttaccct",
];

fn main() {
    let path = std::env::args().nth(1).expect("the input's path");
    let input = std::fs::read_to_string(path).expect("the input");
    let seq = Regex::new(">.*\n|\n").unwrap().replace_all(&input, "").into_owned();
    for v in VARIANTS {
        println!("{} {}", v, Regex::new(v).unwrap().find_iter(&seq).count());
    }
    let mut text = seq.clone();
    for (pattern, with) in [("tHa[Nt]", "<4>"), ("aND|caN|Ha[DS]|WaS", "<3>"), ("a[NSt]|BY", "<2>"), ("<[^>]*>", "|"), (r"\|[^|][^|]*\|", "-")] {
        text = Regex::new(pattern).unwrap().replace_all(&text, NoExpand(with)).into_owned();
    }
    println!("\n{}\n{}\n{}", input.len(), seq.len(), text.len());
}
