// Hashing: 17 MB of access log fed 16 times, 270 MB in all, through SHA-256 and through CRC-32,
// each carried on from one pass to the next. Calx uses std.hash and std.zlib's crc32; Rust the sha2
// and crc32fast crates; Go crypto/sha256 and hash/crc32; Python hashlib and zlib.crc32.
use sha2::{Digest, Sha256};

fn main() {
    let path = std::env::args().nth(1).expect("the input's path");
    let data = std::fs::read(path).unwrap();
    let mut d = Sha256::new();
    for _ in 0..16 {
        d.update(&data);
    }
    let mut crc = crc32fast::Hasher::new();
    for _ in 0..16 {
        crc.update(&data);
    }
    let hex: String = d.finalize().iter().map(|b| format!("{:02x}", b)).collect();
    println!("sha256 {}\ncrc32 {:08x}", hex, crc.finalize());
}
