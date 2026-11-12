# Hashing: 17 MB of access log fed 16 times, 270 MB in all, through SHA-256 and through CRC-32,
# each carried on from one pass to the next. Calx uses std.hash and std.zlib's crc32; Rust the sha2
# and crc32fast crates; Go crypto/sha256 and hash/crc32; Python hashlib and zlib.crc32.
import hashlib
import sys
import zlib


def main():
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    d = hashlib.sha256()
    for _ in range(16):
        d.update(data)
    crc = 0
    for _ in range(16):
        crc = zlib.crc32(data, crc)
    print(f"sha256 {d.hexdigest()}\ncrc32 {crc:08x}")


main()
