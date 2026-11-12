# Compression: 17 MB of access log compressed and decompressed with gzip at level 6 and zstd at
# level 3, and its first 4 MiB with xz at preset 6, each checked against the original. The sizes
# differ by library, so they're printed as `info` lines, which the runner keeps but doesn't
# compare. Calx uses std.zlib, std.zstd and std.xz; Rust flate2, the zstd crate and xz2; Go
# compress/gzip, klauspost/compress and ulikunitz/xz; Python gzip, compression.zstd and lzma.
import gzip
import lzma
import sys
from compression import zstd


def main():
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    gz = gzip.compress(data, compresslevel=6)
    gz_back = gzip.decompress(gz)
    zs = zstd.compress(data, level=3)
    zs_back = zstd.decompress(zs)
    part = data[:4194304]
    x = lzma.compress(part, preset=6)
    x_back = lzma.decompress(x)
    print(f"gzip {str(gz_back == data).lower()}\nzstd {str(zs_back == data).lower()}\nxz {str(x_back == part).lower()}")
    print(f"info gzip {len(gz)}\ninfo zstd {len(zs)}\ninfo xz {len(x)}")


main()
