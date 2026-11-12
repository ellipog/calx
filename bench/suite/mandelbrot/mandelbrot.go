// mandelbrot: the Benchmarks Game's n by n bitmap of the Mandelbrot set, 50 iterations a pixel,
// one bit a pixel, eight to a byte, as a PBM holds it. It prints the bitmap's FNV-1a hash rather
// than the bitmap.
package main

import (
	"fmt"
	"hash/fnv"
	"os"
	"strconv"
)

func main() {
	n := 200
	if len(os.Args) > 1 {
		n, _ = strconv.Atoi(os.Args[1])
	}
	rowBytes := (n + 7) / 8
	bits := make([]byte, rowBytes*n)
	size := float64(n)
	for y := 0; y < n; y++ {
		ci := 2.0*float64(y)/size - 1.0
		var acc byte
		count := 0
		at := y * rowBytes
		for x := 0; x < n; x++ {
			cr := 2.0*float64(x)/size - 1.5
			var zr, zi, tr, ti float64
			for i := 0; i < 50 && tr+ti <= 4.0; i++ {
				zi = 2.0*zr*zi + ci
				zr = tr - ti + cr
				tr = zr * zr
				ti = zi * zi
			}
			acc <<= 1
			if tr+ti <= 4.0 {
				acc |= 1
			}
			count++
			if count == 8 {
				bits[at] = acc
				at++
				acc, count = 0, 0
			} else if x == n-1 {
				bits[at] = acc << (8 - n%8)
				at++
				acc, count = 0, 0
			}
		}
	}
	h := fnv.New64a()
	h.Write(bits)
	fmt.Printf("P4 %d %d\nfnv1a %016x\n", n, n, h.Sum64())
}
