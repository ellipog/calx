# The hashes and checksums of bench/digest/digest.calx, with Python's hashlib (OpenSSL) and zlib,
# in MiB/s over 64 MiB, the best of 5. The bytes are random.Random's rather than digest.calx's
# generator, which pure Python would take half a minute over; these speeds don't depend on what
# the bytes are. Python has no CRC-64 or AES, so those rows are Calx's alone.
import hashlib
import random
import ssl
import sys
import time
import zlib

SIZE = 64 << 20
ROUNDS = 5

data = random.Random(12345).randbytes(SIZE)
steps = [
    ("md5", lambda: hashlib.md5(data).digest()[0]),
    ("sha1", lambda: hashlib.sha1(data).digest()[0]),
    ("sha256", lambda: hashlib.sha256(data).digest()[0]),
    ("sha512", lambda: hashlib.sha512(data).digest()[0]),
    ("crc32", lambda: zlib.crc32(data)),
    ("adler32", lambda: zlib.adler32(data)),
]
best = [float("inf")] * len(steps)
total = 0
for _ in range(ROUNDS):
    for k, (_, step) in enumerate(steps):
        t0 = time.perf_counter_ns()
        total += step()
        best[k] = min(best[k], time.perf_counter_ns() - t0)
print(f"python {sys.version.split()[0]}, {ssl.OPENSSL_VERSION}, zlib {zlib.ZLIB_RUNTIME_VERSION}")
for k, (name, _) in enumerate(steps):
    print(f"{name}: {SIZE / (1 << 20) / (best[k] / 1e9):.0f} MiB/s")
print(f"total {total}")
