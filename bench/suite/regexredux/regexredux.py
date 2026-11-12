# regex-redux: the Benchmarks Game's regular-expression test. fasta's output has its headers and
# line breaks taken out by one pattern, nine variants of a motif counted, and five patterns
# replaced in turn, with the lengths printed. Each language uses its standard regex library, or
# Rust's regex crate; C has none.
import re
import sys

VARIANTS = [
    "agggtaaa|tttaccct",
    "[cgt]gggtaaa|tttaccc[acg]",
    "a[act]ggtaaa|tttacc[agt]t",
    "ag[act]gtaaa|tttac[agt]ct",
    "agg[act]taaa|ttta[agt]cct",
    "aggg[acg]aaa|ttt[cgt]ccct",
    "agggt[cgt]aa|tt[acg]accct",
    "agggta[cgt]a|t[acg]taccct",
    "agggtaa[cgt]|[acg]ttaccct",
]


def main():
    with open(sys.argv[1], newline="") as f:
        text_in = f.read()
    seq = re.sub(">.*\n|\n", "", text_in)
    for v in VARIANTS:
        print(v, len(re.findall(v, seq)))
    text = seq
    for pattern, with_ in [("tHa[Nt]", "<4>"), ("aND|caN|Ha[DS]|WaS", "<3>"), ("a[NSt]|BY", "<2>"), ("<[^>]*>", "|"), (r"\|[^|][^|]*\|", "-")]:
        text = re.sub(pattern, with_, text)
    print(f"\n{len(text_in)}\n{len(seq)}\n{len(text)}")


main()
