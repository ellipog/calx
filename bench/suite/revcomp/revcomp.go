// reverse-complement: the Benchmarks Game's sequence reversal. Each sequence of fasta's output is
// read whole, turned into its reverse complement, every base its pair and in the other order, and
// written 60 to a line under its header.
package main

import (
	"bufio"
	"bytes"
	"os"
)

var pair [256]byte

func flushSequence(out *bufio.Writer, seq []byte) {
	for i, j := 0, len(seq)-1; i < j; i, j = i+1, j-1 {
		seq[i], seq[j] = seq[j], seq[i]
	}
	for i, b := range seq {
		seq[i] = pair[b]
	}
	for at := 0; at < len(seq); at += 60 {
		out.Write(seq[at:min(at+60, len(seq))])
		out.WriteByte('\n')
	}
}

func main() {
	for i := range pair {
		pair[i] = byte(i)
	}
	from, to := "ACGTUMRWSYKVHDBN", "TGCAAKYWSRMBDHVN"
	for i := 0; i < len(from); i++ {
		pair[from[i]] = to[i]
		pair[from[i]+32] = to[i]
	}
	text, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	seq := make([]byte, 0, len(text))
	for _, line := range bytes.Split(text, []byte("\n")) {
		line = bytes.TrimSuffix(line, []byte("\r"))
		if len(line) > 0 && line[0] == '>' {
			if len(seq) > 0 {
				flushSequence(out, seq)
				seq = seq[:0]
			}
			out.Write(line)
			out.WriteByte('\n')
		} else {
			seq = append(seq, line...)
		}
	}
	if len(seq) > 0 {
		flushSequence(out, seq)
	}
}
