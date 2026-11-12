// regex-redux: the Benchmarks Game's regular-expression test. fasta's output has its headers and
// line breaks taken out by one pattern, nine variants of a motif counted, and five patterns
// replaced in turn, with the lengths printed. Each language uses its standard regex library, or
// Rust's regex crate; C has none.
package main

import (
	"fmt"
	"os"
	"regexp"
)

var variants = []string{
	"agggtaaa|tttaccct",
	"[cgt]gggtaaa|tttaccc[acg]",
	"a[act]ggtaaa|tttacc[agt]t",
	"ag[act]gtaaa|tttac[agt]ct",
	"agg[act]taaa|ttta[agt]cct",
	"aggg[acg]aaa|ttt[cgt]ccct",
	"agggt[cgt]aa|tt[acg]accct",
	"agggta[cgt]a|t[acg]taccct",
	"agggtaa[cgt]|[acg]ttaccct",
}

func main() {
	input, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	seq := regexp.MustCompile(">.*\n|\n").ReplaceAllLiteral(input, nil)
	for _, v := range variants {
		fmt.Printf("%s %d\n", v, len(regexp.MustCompile(v).FindAllIndex(seq, -1)))
	}
	text := seq
	for _, s := range [][2]string{{"tHa[Nt]", "<4>"}, {"aND|caN|Ha[DS]|WaS", "<3>"}, {"a[NSt]|BY", "<2>"}, {"<[^>]*>", "|"}, {`\|[^|][^|]*\|`, "-"}} {
		text = regexp.MustCompile(s[0]).ReplaceAllLiteral(text, []byte(s[1]))
	}
	fmt.Printf("\n%d\n%d\n%d\n", len(input), len(seq), len(text))
}
