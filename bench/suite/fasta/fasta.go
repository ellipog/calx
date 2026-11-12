// fasta: the Benchmarks Game's DNA sequence generator. A repeated sequence of 2n bases, then 3n and
// 5n bases drawn from two weighted alphabets with its linear congruential generator, written 60 to
// a line. k-nucleotide, reverse-complement and regex-redux read what it writes.
package main

import (
	"bufio"
	"os"
	"strconv"
)

const alu = "GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTGGGAGGCCGAGGCGGGCGGATCACCTGAGGTCAGGAGTTCGAGACCAGCCTGGCCAACATGGTGAAACCCCGTCTCTACTAAAAATACAAAAATTAGCCGGGCGTGGTGGCGCGCGCCTGTAATCCCAGCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAACCCGGGAGGCGGAGGTTGCAGTGAGCCGAGATCGCGCCACTGCACTCCAGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA"

const width = 60

type Random struct{ last uint32 }

func (r *Random) next() float64 {
	r.last = (r.last*3877 + 29573) % 139968
	return float64(r.last) / 139968.0
}

func repeat(out *bufio.Writer, seq string, n int) {
	k := 0
	line := make([]byte, 0, width+1)
	for n > 0 {
		m := min(n, width)
		line = line[:0]
		for i := 0; i < m; i++ {
			line = append(line, seq[k])
			k++
			if k == len(seq) {
				k = 0
			}
		}
		line = append(line, '\n')
		out.Write(line)
		n -= m
	}
}

func random(out *bufio.Writer, codes string, probs []float64, n int, rng *Random) {
	cumulative := make([]float64, len(probs))
	sum := 0.0
	for i, p := range probs {
		sum += p
		cumulative[i] = sum
	}
	line := make([]byte, 0, width+1)
	for n > 0 {
		m := min(n, width)
		line = line[:0]
		for j := 0; j < m; j++ {
			r := rng.next()
			i := 0
			for i < len(cumulative)-1 && r >= cumulative[i] {
				i++
			}
			line = append(line, codes[i])
		}
		line = append(line, '\n')
		out.Write(line)
		n -= m
	}
}

func main() {
	n := 1000
	if len(os.Args) > 1 {
		n, _ = strconv.Atoi(os.Args[1])
	}
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	rng := &Random{42}
	out.WriteString(">ONE Homo sapiens alu\n")
	repeat(out, alu, n*2)
	out.WriteString(">TWO IUB ambiguity codes\n")
	random(out, "acgtBDHKMNRSVWY", []float64{0.27, 0.12, 0.12, 0.27, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02}, n*3, rng)
	out.WriteString(">THREE Homo sapiens frequency\n")
	random(out, "acgt", []float64{0.3029549426680, 0.1979883004921, 0.1975473066391, 0.3015094502008}, n*5, rng)
}
