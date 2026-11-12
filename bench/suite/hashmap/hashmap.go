// Hash maps: 2 million integer keys inserted, 4 million looked up, half of them missing, every
// other key removed and all looked up again, and the values summed; then 500,000 string keys
// inserted and a million looked up. Each language uses its standard map with its default hash.
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"strconv"
	"strings"
)

func main() {
	dir := os.Args[1]
	ib, err := os.ReadFile(dir + "/ints.bin")
	if err != nil {
		panic(err)
	}
	ints := make([]int64, len(ib)/8)
	for i := range ints {
		ints[i] = int64(binary.LittleEndian.Uint64(ib[i*8:]))
	}
	n, _ := strconv.Atoi(os.Args[2])
	m := make(map[int64]int64)
	for i, k := range ints[:n] {
		m[k] = int64(i)
	}
	var hits, sum int64
	for _, k := range ints[:2*n] {
		if v, ok := m[k]; ok {
			hits++
			sum += v
		}
	}
	fmt.Printf("inserted %d\nfound %d %d\n", len(m), hits, sum)
	for i, k := range ints[:n] {
		if i%2 == 0 {
			delete(m, k)
		}
	}
	hits = 0
	for _, k := range ints[:n] {
		if _, ok := m[k]; ok {
			hits++
		}
	}
	var total int64
	for _, v := range m {
		total += v
	}
	fmt.Printf("left %d %d %d\n", len(m), hits, total)

	text, err := os.ReadFile(dir + "/strings.txt")
	if err != nil {
		panic(err)
	}
	strs := strings.Split(strings.TrimSuffix(string(text), "\n"), "\n")
	s := make(map[string]int64)
	for i, w := range strs[:len(strs)/2] {
		s[w] = int64(i)
	}
	hits, sum = 0, 0
	for _, w := range strs {
		if v, ok := s[w]; ok {
			hits++
			sum += v
		}
	}
	fmt.Printf("strings %d %d %d\n", len(s), hits, sum)
}
