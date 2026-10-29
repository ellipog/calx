// The loops of bench/push/push.calx in Rust, with Vec and the standard allocator.
use std::time::Instant;

fn be32(xs: &[i32]) -> Vec<u8> {
    let mut out = Vec::new();
    for &x in xs {
        let v = x as u32;
        out.push((v >> 24) as u8);
        out.push((v >> 16) as u8);
        out.push((v >> 8) as u8);
        out.push(v as u8);
    }
    out
}

fn varint(xs: &[i32]) -> Vec<u8> {
    let mut out = Vec::new();
    for &x in xs {
        let mut v = (x as u32) & 127;
        while v >= 128 {
            out.push((v & 127) as u8 | 128);
            v >>= 7;
        }
        out.push(v as u8);
    }
    out
}

fn widen(xs: &[i32]) -> Vec<i64> {
    let mut out = Vec::new();
    for &x in xs {
        out.push(x as i64 * 2);
    }
    out
}

fn be32_into(out: &mut Vec<u8>, xs: &[i32]) {
    for &x in xs {
        let v = x as u32;
        out.push((v >> 24) as u8);
        out.push((v >> 16) as u8);
        out.push((v >> 8) as u8);
        out.push(v as u8);
    }
}

fn varint_into(out: &mut Vec<u8>, xs: &[i32]) {
    for &x in xs {
        let mut v = (x as u32) & 127;
        while v >= 128 {
            out.push((v & 127) as u8 | 128);
            v >>= 7;
        }
        out.push(v as u8);
    }
}

fn widen_into(out: &mut Vec<i64>, xs: &[i32]) {
    for &x in xs {
        out.push(x as i64 * 2);
    }
}

fn main() {
    let mut xs = Vec::new();
    let mut s = 1u64;
    for _ in 0..1000000 {
        s = s.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        xs.push((s >> 32) as i32);
    }
    let mut best = [1e6f64; 6];
    let (mut a, mut b, mut c) = (Vec::new(), Vec::new(), Vec::new());
    let mut total = 0usize;
    for _ in 0..50 {
        let t0 = Instant::now();
        let na = be32(&xs);
        let t1 = Instant::now();
        let nb = varint(&xs);
        let t2 = Instant::now();
        let nc = widen(&xs);
        let t3 = Instant::now();
        a.clear();
        be32_into(&mut a, &xs);
        let t4 = Instant::now();
        b.clear();
        varint_into(&mut b, &xs);
        let t5 = Instant::now();
        c.clear();
        widen_into(&mut c, &xs);
        let t6 = Instant::now();
        let ts = [t0, t1, t2, t3, t4, t5, t6];
        for i in 0..6 {
            best[i] = best[i].min((ts[i + 1] - ts[i]).as_secs_f64() * 1000.0);
        }
        total += na.len() + nb.len() + nc.len() + a.len() + b.len() + c.len();
    }
    println!("local: be32 {} ms, varint {} ms, widen {} ms", best[0], best[1], best[2]);
    println!("given: be32 {} ms, varint {} ms, widen {} ms", best[3], best[4], best[5]);
    println!("{total}");
}
