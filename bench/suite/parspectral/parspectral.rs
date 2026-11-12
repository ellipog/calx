// Parallel spectral-norm: the Benchmarks Game's power method at n = 5,500, each of its 40
// matrix-vector products split by rows across the given number of threads. Each element is summed
// in order by one thread, so every language gets the same digits at any thread count. Calx uses
// `parallel for`, Rust Rayon, Go goroutines and C OpenMP.
use rayon::prelude::*;

fn a(i: usize, j: usize) -> f64 {
    let ij = i + j;
    1.0 / (ij * (ij + 1) / 2 + i + 1) as f64
}

fn times(v: &[f64], out: &mut [f64], transposed: bool) {
    out.par_iter_mut().enumerate().for_each(|(i, o)| {
        let mut s = 0.0;
        for (j, x) in v.iter().enumerate() {
            s += if transposed { a(j, i) } else { a(i, j) } * x;
        }
        *o = s;
    });
}

fn main() {
    let mut args = std::env::args().skip(1);
    let n: usize = args.next().and_then(|s| s.parse().ok()).unwrap_or(100);
    let threads: usize = args.next().and_then(|s| s.parse().ok()).unwrap_or(1);
    rayon::ThreadPoolBuilder::new().num_threads(threads).build_global().unwrap();
    let mut u = vec![1.0; n];
    let mut v = vec![0.0; n];
    let mut tmp = vec![0.0; n];
    for _ in 0..10 {
        times(&u, &mut tmp, false);
        times(&tmp, &mut v, true);
        times(&v, &mut tmp, false);
        times(&tmp, &mut u, true);
    }
    let (mut vbv, mut vv) = (0.0f64, 0.0f64);
    for i in 0..n {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    let r = (vbv / vv).sqrt();
    println!("{:.9}\ninfo bits {:016x}", r, r.to_bits());
}
