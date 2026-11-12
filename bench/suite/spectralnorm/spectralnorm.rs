// spectral-norm: the Benchmarks Game's power method for the spectral norm of an infinite matrix,
// cut to n by n, with 10 rounds of u = AᵀA v, v = AᵀA u. Every version sums in the same order.
fn a(i: usize, j: usize) -> f64 {
    let ij = i + j;
    1.0 / (ij * (ij + 1) / 2 + i + 1) as f64
}

fn times(v: &[f64], out: &mut [f64]) {
    for (i, o) in out.iter_mut().enumerate() {
        let mut s = 0.0;
        for (j, x) in v.iter().enumerate() {
            s += a(i, j) * x;
        }
        *o = s;
    }
}

fn times_transposed(v: &[f64], out: &mut [f64]) {
    for (i, o) in out.iter_mut().enumerate() {
        let mut s = 0.0;
        for (j, x) in v.iter().enumerate() {
            s += a(j, i) * x;
        }
        *o = s;
    }
}

fn times_ata(v: &[f64], out: &mut [f64], tmp: &mut [f64]) {
    times(v, tmp);
    times_transposed(tmp, out);
}

fn main() {
    let n: usize = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(100);
    let mut u = vec![1.0; n];
    let mut v = vec![0.0; n];
    let mut tmp = vec![0.0; n];
    for _ in 0..10 {
        times_ata(&u, &mut v, &mut tmp);
        times_ata(&v, &mut u, &mut tmp);
    }
    let mut vbv = 0.0;
    let mut vv = 0.0;
    for i in 0..n {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    println!("{:.9}", (vbv / vv).sqrt());
}
