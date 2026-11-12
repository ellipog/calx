# reverse-complement: the Benchmarks Game's sequence reversal. Each sequence of fasta's output is
# read whole, turned into its reverse complement, every base its pair and in the other order, and
# written 60 to a line under its header.
import sys

PAIRS = bytes.maketrans(b"ACGTUMRWSYKVHDBNacgtumrwsykvhdbn", b"TGCAAKYWSRMBDHVNTGCAAKYWSRMBDHVN")


def flush_sequence(out, parts):
    seq = b"".join(parts).translate(PAIRS)[::-1]
    for at in range(0, len(seq), 60):
        out.write(seq[at : at + 60])
        out.write(b"\n")


def main():
    out = sys.stdout.buffer
    parts = []
    with open(sys.argv[1], "rb") as f:
        for line in f:
            line = line.rstrip(b"\r\n")
            if line.startswith(b">"):
                if parts:
                    flush_sequence(out, parts)
                    parts = []
                out.write(line + b"\n")
            else:
                parts.append(line)
    if parts:
        flush_sequence(out, parts)


main()
