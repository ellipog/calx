// Many files: 2,000 files of 1 to 16 KB written into 20 folders, read back and their bytes added
// up, then removed with their folders, through the language's own file functions.
package main

import (
	"fmt"
	"os"
	"strconv"
)

func main() {
	root := os.Args[1] + "/run"
	files, _ := strconv.Atoi(os.Args[2])
	pattern := make([]byte, 16384+251)
	for j := range pattern {
		pattern[j] = byte(j % 251)
	}
	check := func(err error) {
		if err != nil {
			panic(err)
		}
	}
	check(os.Mkdir(root, 0o755))
	for d := 0; d < 20; d++ {
		check(os.Mkdir(fmt.Sprintf("%s/d%d", root, d), 0o755))
	}
	written := 0
	for i := 0; i < files; i++ {
		size := 1024 + (i*7919)%15000
		from := i % 251
		check(os.WriteFile(fmt.Sprintf("%s/d%d/f%d.bin", root, i%20, i), pattern[from:from+size], 0o644))
		written += size
	}
	var sum uint64
	read := 0
	for i := 0; i < files; i++ {
		data, err := os.ReadFile(fmt.Sprintf("%s/d%d/f%d.bin", root, i%20, i))
		check(err)
		read += len(data)
		for _, b := range data {
			sum += uint64(b)
		}
	}
	for i := 0; i < files; i++ {
		check(os.Remove(fmt.Sprintf("%s/d%d/f%d.bin", root, i%20, i)))
	}
	for d := 0; d < 20; d++ {
		check(os.Remove(fmt.Sprintf("%s/d%d", root, d)))
	}
	check(os.Remove(root))
	fmt.Printf("files %d\nwritten %d\nread %d\nsum %d\n", files, written, read, sum)
}
