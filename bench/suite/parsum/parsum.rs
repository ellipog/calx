// Parallel float sum: π by the midpoint rule, 4 / (1 + x²) over n strips, the classic OpenMP
// reduction example, summed on the given number of threads. Rust uses Rayon's `sum`, which adds
// partial sums in an order that depends on how the work was split; Calx gives each of 1,024
// chunks its own sum, C uses an OpenMP `reduction`, and Go one partial sum per goroutine.
use rayon::prelude::*;

fn main() {
    let mut args = std::env::args().skip(1);
    let n: usize = args.next().and_then(|s| s.parse().ok()).unwrap_or(1000);
    let threads: usize = args.next().and_then(|s| s.parse().ok()).unwrap_or(1);
    rayon::ThreadPoolBuilder::new().num_threads(threads).build_global().unwrap();
    let step = 1.0 / n as f64;
    let total: f64 = (0..n)
        .into_par_iter()
        .map(|i| {
            let x = (i as f64 + 0.5) * step;
            4.0 / (1.0 + x * x)
        })
        .sum();
    let pi = total * step;
    println!("{:.6}\ninfo bits {:016x}", pi, pi.to_bits());
}
