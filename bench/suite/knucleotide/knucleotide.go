// k-nucleotide: the Benchmarks Game's hash-table test. The third sequence of fasta's output is read,
// every k-mer of it counted for each k, and the 1- and 2-mers' frequencies and five longer k-mers'
// counts printed. Each k-mer is the key, two bits a base in a 64-bit integer, of the language's
// own hash map.
package main

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"sort"
)

func code(b byte) uint64 {
	switch b {
	case 'A':
		return 0
	case 'C':
		return 1
	case 'G':
		return 2
	}
	return 3
}

func count(seq []byte, k int) map[uint64]int32 {
	counts := make(map[uint64]int32)
	mask := uint64(1)<<(2*k) - 1
	var key uint64
	for _, b := range seq[:k-1] {
		key = key<<2 | code(b)
	}
	for _, b := range seq[k-1:] {
		key = (key<<2 | code(b)) & mask
		counts[key]++
	}
	return counts
}

func decode(key uint64, k int) string {
	s := make([]byte, k)
	for i := 0; i < k; i++ {
		s[i] = "ACGT"[(key>>(2*(k-1-i)))&3]
	}
	return string(s)
}

type entry struct {
	key uint64
	n   int32
}

func frequencies(out *bufio.Writer, seq []byte, k int) {
	counts := count(seq, k)
	entries := make([]entry, 0, len(counts))
	for key, n := range counts {
		entries = append(entries, entry{key, n})
	}
	sort.Slice(entries, func(i, j int) bool {
		if entries[i].n != entries[j].n {
			return entries[i].n > entries[j].n
		}
		return entries[i].key < entries[j].key
	})
	total := float64(len(seq) - k + 1)
	for _, e := range entries {
		fmt.Fprintf(out, "%s %.3f\n", decode(e.key, k), 100.0*float64(e.n)/total)
	}
	fmt.Fprintln(out)
}

func occurrences(out *bufio.Writer, seq []byte, kmer string) {
	counts := count(seq, len(kmer))
	var key uint64
	for i := 0; i < len(kmer); i++ {
		key = key<<2 | code(kmer[i])
	}
	fmt.Fprintf(out, "%d\t%s\n", counts[key], kmer)
}

func main() {
	text, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	var seq []byte
	inside := false
	for _, line := range bytes.Split(text, []byte("\n")) {
		line = bytes.TrimRight(line, "\r")
		if len(line) > 0 && line[0] == '>' {
			if inside {
				break
			}
			inside = bytes.HasPrefix(line, []byte(">THREE"))
		} else if inside {
			seq = append(seq, bytes.ToUpper(line)...)
		}
	}
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	frequencies(out, seq, 1)
	frequencies(out, seq, 2)
	for _, kmer := range []string{"GGT", "GGTA", "GGTATT", "GGTATTTTAATT", "GGTATTTTAATTTATAGT"} {
		occurrences(out, seq, kmer)
	}
}
