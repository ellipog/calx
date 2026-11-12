// Word count: 5 million words of text, about 30 MB, lowercased, split into runs of ASCII letters,
// counted in the language's hash map, and the 20 most common printed, ties by the word.
package main

import (
	"bytes"
	"fmt"
	"os"
	"sort"
)

func main() {
	data, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	text := bytes.ToLower(data)
	words := bytes.FieldsFunc(text, func(r rune) bool { return r < 'a' || r > 'z' })
	counts := make(map[string]int32)
	for _, w := range words {
		counts[string(w)]++
	}
	type entry struct {
		word string
		n    int32
	}
	top := make([]entry, 0, len(counts))
	for w, n := range counts {
		top = append(top, entry{w, n})
	}
	sort.Slice(top, func(i, j int) bool {
		if top[i].n != top[j].n {
			return top[i].n > top[j].n
		}
		return top[i].word < top[j].word
	})
	fmt.Printf("words %d\ndistinct %d\n", len(words), len(counts))
	for _, e := range top[:min(20, len(top))] {
		fmt.Printf("%d %s\n", e.n, e.word)
	}
}
