// The loops of bench/push/push.calx in Rust, with Vec and the standard allocator.
use std::hint::black_box;
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

struct Writer {
    data: Vec<u8>,
    #[allow(dead_code)]
    problem: Option<String>,
}

impl Writer {
    fn i32(&mut self, v: i32) {
        let v = v as u32;
        self.data.push((v >> 24) as u8);
        self.data.push((v >> 16) as u8);
        self.data.push((v >> 8) as u8);
        self.data.push(v as u8);
    }

    fn varint(&mut self, v: i32) {
        let mut x = v as u32;
        while (x & !127) != 0 {
            self.data.push(((x & 0x7F) | 0x80) as u8);
            x >>= 7;
        }
        self.data.push(x as u8);
    }
}

fn encode(w: &mut Writer, xs: &[i32]) {
    for &x in xs {
        w.i32(x);
        w.varint(x & 0x3FFF);
    }
}

fn main() {
    let mut xs = Vec::new();
    let mut s = 1u64;
    for _ in 0..1000000 {
        s = s.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        xs.push((s >> 32) as i32);
    }
    let mut best = [1e6f64; 10];
    let be32_far = black_box(be32_into as fn(&mut Vec<u8>, &[i32]));
    let varint_far = black_box(varint_into as fn(&mut Vec<u8>, &[i32]));
    let widen_far = black_box(widen_into as fn(&mut Vec<i64>, &[i32]));
    let encode_far = black_box(encode as fn(&mut Writer, &[i32]));
    let mut w = Writer { data: Vec::new(), problem: None };
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
        a.clear();
        be32_far(&mut a, &xs);
        let t7 = Instant::now();
        b.clear();
        varint_far(&mut b, &xs);
        let t8 = Instant::now();
        c.clear();
        widen_far(&mut c, &xs);
        let t9 = Instant::now();
        w.data.clear();
        encode_far(&mut w, &xs);
        let t10 = Instant::now();
        let ts = [t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10];
        for i in 0..10 {
            best[i] = best[i].min((ts[i + 1] - ts[i]).as_secs_f64() * 1000.0);
        }
        total += na.len() + nb.len() + nc.len() + a.len() + b.len() + c.len() + w.data.len();
    }
    println!("local: be32 {} ms, varint {} ms, widen {} ms", best[0], best[1], best[2]);
    println!("given: be32 {} ms, varint {} ms, widen {} ms", best[3], best[4], best[5]);
    println!("given, not inlined: be32 {} ms, varint {} ms, widen {} ms", best[6], best[7], best[8]);
    println!("writer, not inlined: {} ms", best[9]);
    println!("{total}");
}
