// fasta: the Benchmarks Game's DNA sequence generator. A repeated sequence of 2n bases, then 3n and
// 5n bases drawn from two weighted alphabets with its linear congruential generator, written 60 to
// a line. k-nucleotide, reverse-complement and regex-redux read what it writes.
use std::io::{self, BufWriter, Write};

const ALU: &[u8] = b"GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTGGGAGGCCGAGGCGGGCGGATCACCTGAGGTCAGGAGTTCGAGACCAGCCTGGCCAACATGGTGAAACCCCGTCTCTACTAAAAATACAAAAATTAGCCGGGCGTGGTGGCGCGCGCCTGTAATCCCAGCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAACCCGGGAGGCGGAGGTTGCAGTGAGCCGAGATCGCGCCACTGCACTCCAGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA";
const WIDTH: usize = 60;

struct Random {
    last: u32,
}

impl Random {
    fn next(&mut self) -> f64 {
        self.last = (self.last * 3877 + 29573) % 139968;
        self.last as f64 / 139968.0
    }
}

fn repeat(out: &mut impl Write, seq: &[u8], mut n: usize) -> io::Result<()> {
    let mut k = 0;
    let mut line = Vec::with_capacity(WIDTH + 1);
    while n > 0 {
        let m = n.min(WIDTH);
        line.clear();
        for _ in 0..m {
            line.push(seq[k]);
            k += 1;
            if k == seq.len() {
                k = 0;
            }
        }
        line.push(b'\n');
        out.write_all(&line)?;
        n -= m;
    }
    Ok(())
}

fn random(out: &mut impl Write, codes: &[u8], probs: &[f64], mut n: usize, rng: &mut Random) -> io::Result<()> {
    let mut sum = 0.0;
    let cumulative: Vec<f64> = probs
        .iter()
        .map(|p| {
            sum += p;
            sum
        })
        .collect();
    let mut line = Vec::with_capacity(WIDTH + 1);
    while n > 0 {
        let m = n.min(WIDTH);
        line.clear();
        for _ in 0..m {
            let r = rng.next();
            let mut i = 0;
            while i < cumulative.len() - 1 && r >= cumulative[i] {
                i += 1;
            }
            line.push(codes[i]);
        }
        line.push(b'\n');
        out.write_all(&line)?;
        n -= m;
    }
    Ok(())
}

fn main() -> io::Result<()> {
    let n: usize = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(1000);
    let stdout = io::stdout();
    let mut out = BufWriter::new(stdout.lock());
    let mut rng = Random { last: 42 };
    out.write_all(b">ONE Homo sapiens alu\n")?;
    repeat(&mut out, ALU, n * 2)?;
    out.write_all(b">TWO IUB ambiguity codes\n")?;
    random(&mut out, b"acgtBDHKMNRSVWY", &[0.27, 0.12, 0.12, 0.27, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02], n * 3, &mut rng)?;
    out.write_all(b">THREE Homo sapiens frequency\n")?;
    random(&mut out, b"acgt", &[0.3029549426680, 0.1979883004921, 0.1975473066391, 0.3015094502008], n * 5, &mut rng)?;
    out.flush()
}
