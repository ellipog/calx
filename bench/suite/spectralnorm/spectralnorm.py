# spectral-norm: the Benchmarks Game's power method for the spectral norm of an infinite matrix,
# cut to n by n, with 10 rounds of u = AᵀA v, v = AᵀA u. Every version sums in the same order: a
# plain loop, since sum() of floats rounds differently since Python 3.12.
import math
import sys


def a(i, j):
    ij = i + j
    return 1.0 / (ij * (ij + 1) // 2 + i + 1)


def times(v, out):
    n = len(v)
    for i in range(n):
        s = 0.0
        for j in range(n):
            s += a(i, j) * v[j]
        out[i] = s


def times_transposed(v, out):
    n = len(v)
    for i in range(n):
        s = 0.0
        for j in range(n):
            s += a(j, i) * v[j]
        out[i] = s


def times_ata(v, out, tmp):
    times(v, tmp)
    times_transposed(tmp, out)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    u = [1.0] * n
    v = [0.0] * n
    tmp = [0.0] * n
    for _ in range(10):
        times_ata(u, v, tmp)
        times_ata(v, u, tmp)
    vbv = vv = 0.0
    for i in range(n):
        vbv += u[i] * v[i]
        vv += v[i] * v[i]
    print(f"{math.sqrt(vbv / vv):.9f}")


main()
