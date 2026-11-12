// reverse-complement: the Benchmarks Game's sequence reversal. Each sequence of fasta's output is
// read whole, turned into its reverse complement, every base its pair and in the other order, and
// written 60 to a line under its header.
use std::io::{self, BufWriter, Write};

fn flush_sequence(out: &mut impl Write, seq: &mut Vec<u8>, pair: &[u8; 256]) -> io::Result<()> {
    seq.reverse();
    for b in seq.iter_mut() {
        *b = pair[*b as usize];
    }
    for line in seq.chunks(60) {
        out.write_all(line)?;
        out.write_all(b"\n")?;
    }
    seq.clear();
    Ok(())
}

fn main() -> io::Result<()> {
    let mut pair = [0u8; 256];
    for (i, p) in pair.iter_mut().enumerate() {
        *p = i as u8;
    }
    for (&from, &to) in b"ACGTUMRWSYKVHDBN".iter().zip(b"TGCAAKYWSRMBDHVN") {
        pair[from as usize] = to;
        pair[from.to_ascii_lowercase() as usize] = to;
    }
    let path = std::env::args().nth(1).expect("the input's path");
    let text = std::fs::read(path)?;
    let stdout = io::stdout();
    let mut out = BufWriter::new(stdout.lock());
    let mut seq = Vec::with_capacity(text.len());
    for line in text.split(|&b| b == b'\n') {
        let line = line.strip_suffix(b"\r").unwrap_or(line);
        if line.first() == Some(&b'>') {
            if !seq.is_empty() {
                flush_sequence(&mut out, &mut seq, &pair)?;
            }
            out.write_all(line)?;
            out.write_all(b"\n")?;
        } else {
            seq.extend_from_slice(line);
        }
    }
    if !seq.is_empty() {
        flush_sequence(&mut out, &mut seq, &pair)?;
    }
    out.flush()
}
