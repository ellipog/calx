// mandelbrot: the Benchmarks Game's n by n bitmap of the Mandelbrot set, 50 iterations a pixel,
// one bit a pixel, eight to a byte, as a PBM holds it. It prints the bitmap's FNV-1a hash rather
// than the bitmap.
fn main() {
    let n: usize = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(200);
    let row_bytes = (n + 7) / 8;
    let mut bits = vec![0u8; row_bytes * n];
    let size = n as f64;
    for y in 0..n {
        let ci = 2.0 * y as f64 / size - 1.0;
        let row = &mut bits[y * row_bytes..(y + 1) * row_bytes];
        let mut acc = 0u8;
        let mut count = 0;
        let mut at = 0;
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
                row[at] = acc;
                at += 1;
                acc = 0;
                count = 0;
            } else if x == n - 1 {
                row[at] = acc << (8 - n % 8);
                at += 1;
                acc = 0;
                count = 0;
            }
        }
    }
    let mut h: u64 = 0xcbf29ce484222325;
    for &b in &bits {
        h = (h ^ b as u64).wrapping_mul(0x100000001b3);
    }
    println!("P4 {} {}\nfnv1a {:016x}", n, n, h);
}
