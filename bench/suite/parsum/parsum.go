// Parallel float sum: π by the midpoint rule, 4 / (1 + x²) over n strips, the classic OpenMP
// reduction example, summed on the given number of threads. Go gives each of GOMAXPROCS
// goroutines an equal run of strips and adds their partial sums, so the order depends on the
// thread count; Calx gives each of 1,024 chunks its own sum, Rust uses Rayon's `sum`, and C an
// OpenMP `reduction`.
package main

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"strconv"
	"sync"
)

func main() {
	n, _ := strconv.Atoi(os.Args[1])
	threads, _ := strconv.Atoi(os.Args[2])
	runtime.GOMAXPROCS(threads)
	step := 1.0 / float64(n)
	sums := make([]float64, threads)
	var wg sync.WaitGroup
	for w := 0; w < threads; w++ {
		lo, hi := w*n/threads, (w+1)*n/threads
		wg.Add(1)
		go func() {
			defer wg.Done()
			s := 0.0
			for i := lo; i < hi; i++ {
				x := (float64(i) + 0.5) * step
				s += 4.0 / (1.0 + x*x)
			}
			sums[w] = s
		}()
	}
	wg.Wait()
	total := 0.0
	for _, s := range sums {
		total += s
	}
	pi := total * step
	fmt.Printf("%.6f\ninfo bits %016x\n", pi, math.Float64bits(pi))
}
