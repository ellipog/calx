# fasta: the Benchmarks Game's DNA sequence generator. A repeated sequence of 2n bases, then 3n and
# 5n bases drawn from two weighted alphabets with its linear congruential generator, written 60 to
# a line. k-nucleotide, reverse-complement and regex-redux read what it writes.
import sys

ALU = (
    "GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTGGGAGGCCGAGGCGGGCGGATCACCTGAGGTCAGGAGTTCGAGACCAGCCTGGCCAACATGGTGAAACCCC"
    "GTCTCTACTAAAAATACAAAAATTAGCCGGGCGTGGTGGCGCGCGCCTGTAATCCCAGCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAACCCGGGAGGCGGAGGTTGC"
    "AGTGAGCCGAGATCGCGCCACTGCACTCCAGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA"
)
WIDTH = 60


class Random:
    def __init__(self):
        self.last = 42

    def next(self):
        self.last = (self.last * 3877 + 29573) % 139968
        return self.last / 139968.0


def repeat(out, seq, n):
    k = 0
    while n > 0:
        m = min(n, WIDTH)
        line = []
        for _ in range(m):
            line.append(seq[k])
            k += 1
            if k == len(seq):
                k = 0
        out.write("".join(line) + "\n")
        n -= m


def random(out, codes, probs, n, rng):
    cumulative = []
    total = 0.0
    for p in probs:
        total += p
        cumulative.append(total)
    last = len(cumulative) - 1
    while n > 0:
        m = min(n, WIDTH)
        line = []
        for _ in range(m):
            r = rng.next()
            i = 0
            while i < last and r >= cumulative[i]:
                i += 1
            line.append(codes[i])
        out.write("".join(line) + "\n")
        n -= m


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
    out = sys.stdout
    rng = Random()
    out.write(">ONE Homo sapiens alu\n")
    repeat(out, ALU, n * 2)
    out.write(">TWO IUB ambiguity codes\n")
    random(out, "acgtBDHKMNRSVWY", [0.27, 0.12, 0.12, 0.27] + [0.02] * 11, n * 3, rng)
    out.write(">THREE Homo sapiens frequency\n")
    random(out, "acgt", [0.3029549426680, 0.1979883004921, 0.1975473066391, 0.3015094502008], n * 5, rng)


main()
