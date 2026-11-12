// Parallel mandelbrot: the Benchmarks Game's bitmap, 6,000 by 6,000, its rows computed at once on
// the given number of threads, and the bitmap's FNV-1a hash printed. Calx uses `parallel for`,
// Rust Rayon, Go goroutines and C OpenMP.
use rayon::prelude::*;

fn row(y: usize, n: usize, bits: &mut [u8]) {
    let size = n as f64;
    let ci = 2.0 * y as f64 / size - 1.0;
    let (mut acc, mut count, mut at) = (0u8, 0, 0);
    for x in 0..n {
        let cr = 2.0 * x as f64 / size - 1.5;
        let (mut zr, mut zi, mut tr, mut ti) = (0.0f64, 0.0f64, 0.0f64, 0.0f64);
        let mut i = 0;
        while i < 50 && tr + ti <= 4.0 {
            zi = 2.0 * zr * zi + ci;
            zr = tr - ti + cr;
            tr = zr * zr;
            ti = zi * zi;
            i += 1;
        }
        acc <<= 1;
        if tr + ti <= 4.0 {
            acc |= 1;
        }
        count += 1;
        if count == 8 {
            bits[at] = acc;
            at += 1;
            acc = 0;
            count = 0;
        } else if x == n - 1 {
            bits[at] = acc << (8 - n % 8);
        }
    }
}

fn main() {
    let mut args = std::env::args().skip(1);
    let n: usize = args.next().and_then(|s| s.parse().ok()).unwrap_or(200);
    let threads: usize = args.next().and_then(|s| s.parse().ok()).unwrap_or(1);
    rayon::ThreadPoolBuilder::new().num_threads(threads).build_global().unwrap();
    let row_bytes = (n + 7) / 8;
    let mut bits = vec![0u8; row_bytes * n];
    bits.par_chunks_mut(row_bytes).enumerate().for_each(|(y, r)| row(y, n, r));
    let mut h: u64 = 0xcbf29ce484222325;
    for &b in &bits {
        h = (h ^ b as u64).wrapping_mul(0x100000001b3);
    }
    println!("P4 {} {}\nfnv1a {:016x}", n, n, h);
}
