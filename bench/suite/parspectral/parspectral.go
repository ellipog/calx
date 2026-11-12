// Parallel spectral-norm: the Benchmarks Game's power method at n = 5,500, each of its 40
// matrix-vector products split by rows across the given number of threads. Each element is summed
// in order by one thread, so every language gets the same digits at any thread count. Calx uses
// `parallel for`, Rust Rayon, Go goroutines and C OpenMP. Here each of GOMAXPROCS goroutines
// takes an equal run of rows.
package main

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"strconv"
	"sync"
)

func a(i, j int) float64 {
	ij := i + j
	return 1.0 / float64(ij*(ij+1)/2+i+1)
}

func times(v, out []float64, transposed bool, threads int) {
	n := len(v)
	var wg sync.WaitGroup
	for w := 0; w < threads; w++ {
		lo, hi := w*n/threads, (w+1)*n/threads
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := lo; i < hi; i++ {
				s := 0.0
				for j, x := range v {
					if transposed {
						s += a(j, i) * x
					} else {
						s += a(i, j) * x
					}
				}
				out[i] = s
			}
		}()
	}
	wg.Wait()
}

func main() {
	n, _ := strconv.Atoi(os.Args[1])
	threads, _ := strconv.Atoi(os.Args[2])
	runtime.GOMAXPROCS(threads)
	u := make([]float64, n)
	v := make([]float64, n)
	tmp := make([]float64, n)
	for i := range u {
		u[i] = 1.0
	}
	for k := 0; k < 10; k++ {
		times(u, tmp, false, threads)
		times(tmp, v, true, threads)
		times(v, tmp, false, threads)
		times(tmp, u, true, threads)
	}
	var vbv, vv float64
	for i := 0; i < n; i++ {
		vbv += u[i] * v[i]
		vv += v[i] * v[i]
	}
	r := math.Sqrt(vbv / vv)
	fmt.Printf("%.9f\ninfo bits %016x\n", r, math.Float64bits(r))
}
