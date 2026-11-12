// Sorting: 5 million 64-bit integers and 5 million doubles read from binary files, and a million
// strings read a line each, each list sorted with the language's own sort and eleven evenly
// spaced elements printed.
package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"slices"
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
	fb, err := os.ReadFile(dir + "/floats.bin")
	if err != nil {
		panic(err)
	}
	floats := make([]float64, len(fb)/8)
	for i := range floats {
		floats[i] = math.Float64frombits(binary.LittleEndian.Uint64(fb[i*8:]))
	}
	text, err := os.ReadFile(dir + "/strings.txt")
	if err != nil {
		panic(err)
	}
	strs := strings.Split(strings.TrimSuffix(string(text), "\n"), "\n")

	slices.Sort(ints)
	slices.Sort(floats)
	slices.Sort(strs)

	for k := 0; k <= 10; k++ {
		i := min(k*len(ints)/10, len(ints)-1)
		f := min(k*len(floats)/10, len(floats)-1)
		s := min(k*len(strs)/10, len(strs)-1)
		fmt.Printf("%d %.6f %s\n", ints[i], floats[f], strs[s])
	}
}
