// Building and taking apart text: a million CSV-like lines built onto one string with formatting,
// then each line split at its commas, a field uppercased and three joined with " | " onto a second
// string, the lines that name one user counted, and a word replaced through the whole result.
package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
)

func main() {
	n, _ := strconv.ParseInt(os.Args[1], 10, 64)
	words := []string{"alpha", "beta", "gamma", "delta", "epsilon"}
	var b strings.Builder
	for i := int64(0); i < n; i++ {
		fmt.Fprintf(&b, "%d,user%d,%d.%02d,%s\n", i, i%1000, i*7%10007, i%100, words[i%5])
	}
	built := b.String()
	var o strings.Builder
	matches := 0
	for _, line := range strings.Split(strings.TrimSuffix(built, "\n"), "\n") {
		fields := strings.Split(line, ",")
		o.WriteString(strings.ToUpper(fields[3]))
		o.WriteString(" | ")
		o.WriteString(fields[1])
		o.WriteString(" | ")
		o.WriteString(fields[2])
		o.WriteByte('\n')
		if fields[1] == "user42" {
			matches++
		}
	}
	out := o.String()
	replaced := strings.ReplaceAll(out, "user", "u")
	fmt.Printf("built %d\nout %d\nmatches %d\nreplaced %d\n", len(built), len(out), matches, len(replaced))
}
