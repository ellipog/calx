// Parallel mandelbrot: the Benchmarks Game's bitmap, 6,000 by 6,000, its rows computed at once on
// the given number of threads, and the bitmap's FNV-1a hash printed. Calx uses `parallel for`,
// Rust Rayon, Go goroutines and C OpenMP. Here each of GOMAXPROCS goroutines takes the next row
// from a shared counter.
package main

import (
	"fmt"
	"hash/fnv"
	"os"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
)

func row(y, n int, bits []byte) {
	size := float64(n)
	ci := 2.0*float64(y)/size - 1.0
	var acc byte
	count, at := 0, 0
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
		}
	}
}

func main() {
	n, _ := strconv.Atoi(os.Args[1])
	threads, _ := strconv.Atoi(os.Args[2])
	runtime.GOMAXPROCS(threads)
	rowBytes := (n + 7) / 8
	bits := make([]byte, rowBytes*n)
	var next atomic.Int64
	var wg sync.WaitGroup
	for w := 0; w < threads; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				y := int(next.Add(1) - 1)
				if y >= n {
					return
				}
				row(y, n, bits[y*rowBytes:(y+1)*rowBytes])
			}
		}()
	}
	wg.Wait()
	h := fnv.New64a()
	h.Write(bits)
	fmt.Printf("P4 %d %d\nfnv1a %016x\n", n, n, h.Sum64())
}
