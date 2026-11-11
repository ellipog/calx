"""bench/format/format.calx with Python's f-strings, each string made with "".join, the quickest way
Python has to put many pieces together."""
import time

MASK = (1 << 64) - 1


def main():
    x = 88172645463325252
    ints = []
    doubles = []
    for k in range(1000000):
        x ^= (x << 13) & MASK
        x ^= x >> 7
        x ^= (x << 17) & MASK
        ints.append(x >> 44)
        if k < 200000:
            doubles.append(1.0 + (x >> 11) * 999.0 / 9007199254740992.0)
    fixed = padded = plain = shortest = float("inf")
    total = 0
    head = ints[:200000]
    for _ in range(7):
        t0 = time.perf_counter()
        a = "".join([f"{v:.3f}," for v in doubles])
        t1 = time.perf_counter()
        b = "".join([f"{i:>8}," for i in head])
        t2 = time.perf_counter()
        c = "".join([f"{n}," for n in ints])
        t3 = time.perf_counter()
        d = "".join([f"{v}," for v in doubles])
        t4 = time.perf_counter()
        total += len(a) + len(b) + len(c) + len(d)
        fixed = min(fixed, t1 - t0)
        padded = min(padded, t2 - t1)
        plain = min(plain, t3 - t2)
        shortest = min(shortest, t4 - t3)
    print("python: fixed %d us, padded %d us, integers %d us, shortest %d us, total %d"
          % (fixed * 1e6, padded * 1e6, plain * 1e6, shortest * 1e6, total))


main()
