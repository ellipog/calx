# k-nucleotide: the Benchmarks Game's hash-table test. The third sequence of fasta's output is read,
# every k-mer of it counted for each k, and the 1- and 2-mers' frequencies and five longer k-mers'
# counts printed. Python counts the k-mers as text, with a Counter, which is how Python counts
# them; the compiled versions pack each into an integer.
import sys
from collections import Counter


def count(seq, k):
    return Counter(seq[i : i + k] for i in range(len(seq) - k + 1))


def frequencies(seq, k):
    counts = count(seq, k)
    total = len(seq) - k + 1
    for kmer, n in sorted(counts.items(), key=lambda e: (-e[1], e[0])):
        print(f"{kmer} {100.0 * n / total:.3f}")
    print()


def main():
    seq = []
    inside = False
    with open(sys.argv[1]) as f:
        for line in f:
            if line.startswith(">"):
                if inside:
                    break
                inside = line.startswith(">THREE")
            elif inside:
                seq.append(line.rstrip("\n").upper())
    seq = "".join(seq)
    frequencies(seq, 1)
    frequencies(seq, 2)
    for kmer in ["GGT", "GGTA", "GGTATT", "GGTATTTTAATT", "GGTATTTTAATTTATAGT"]:
        print(f"{count(seq, len(kmer))[kmer]}\t{kmer}")


main()
