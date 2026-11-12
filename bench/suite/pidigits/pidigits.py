# pidigits: the Benchmarks Game's digits of π from the unbounded spigot, ten to a line, with
# integers of any size. Each language uses its own big integers: std.bigint in Calx, num-bigint
# in Rust, math/big in Go and Python's int here. C has none in its standard library.
import sys


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 27
    numer, accum, denom = 1, 0, 1
    line = []
    i = k = 0
    while i < n:
        k += 1
        k2 = k * 2 + 1
        accum = (accum + numer * 2) * k2
        denom *= k2
        numer *= k
        if numer > accum:
            continue
        d = (numer * 3 + accum) // denom
        if d != (numer * 4 + accum) // denom:
            continue
        line.append(str(d))
        i += 1
        if i % 10 == 0:
            print(f"{''.join(line)}\t:{i}")
            line = []
        accum = (accum - denom * d) * 10
        numer *= 10
    if line:
        print(f"{''.join(line):<10}\t:{n}")


main()
