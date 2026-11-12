// Compression: 17 MB of access log compressed and decompressed with gzip at level 6 and zstd at
// level 3, and its first 4 MiB with xz at preset 6, each checked against the original. The sizes
// differ by library, so they're printed as `info` lines, which the runner keeps but doesn't
// compare. Calx uses std.zlib, std.zstd and std.xz; Rust flate2, the zstd crate and xz2; Go
// compress/gzip, klauspost/compress and ulikunitz/xz; Python gzip, compression.zstd and lzma.
use std::io::{Read, Write};

fn main() -> std::io::Result<()> {
    let path = std::env::args().nth(1).expect("the input's path");
    let data = std::fs::read(path)?;

    let mut enc = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::new(6));
    enc.write_all(&data)?;
    let gz = enc.finish()?;
    let mut gz_back = Vec::new();
    flate2::read::GzDecoder::new(&gz[..]).read_to_end(&mut gz_back)?;

    let zs = zstd::encode_all(&data[..], 3)?;
    let zs_back = zstd::decode_all(&zs[..])?;

    let part = &data[..data.len().min(4194304)];
    let mut x = Vec::new();
    xz2::read::XzEncoder::new(part, 6).read_to_end(&mut x)?;
    let mut x_back = Vec::new();
    xz2::read::XzDecoder::new(&x[..]).read_to_end(&mut x_back)?;

    println!("gzip {}\nzstd {}\nxz {}", gz_back == data, zs_back == data, x_back == part);
    println!("info gzip {}\ninfo zstd {}\ninfo xz {}", gz.len(), zs.len(), x.len());
    Ok(())
}
