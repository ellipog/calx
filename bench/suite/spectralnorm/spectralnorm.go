// spectral-norm: the Benchmarks Game's power method for the spectral norm of an infinite matrix,
// cut to n by n, with 10 rounds of u = AᵀA v, v = AᵀA u. Every version sums in the same order.
package main

import (
	"fmt"
	"math"
	"os"
	"strconv"
)

func a(i, j int) float64 {
	ij := i + j
	return 1.0 / float64(ij*(ij+1)/2+i+1)
}

func times(v, out []float64) {
	for i := range out {
		s := 0.0
		for j, x := range v {
			s += a(i, j) * x
		}
		out[i] = s
	}
}

func timesTransposed(v, out []float64) {
	for i := range out {
		s := 0.0
		for j, x := range v {
			s += a(j, i) * x
		}
		out[i] = s
	}
}

func timesAtA(v, out, tmp []float64) {
	times(v, tmp)
	timesTransposed(tmp, out)
}

func main() {
	n := 100
	if len(os.Args) > 1 {
		n, _ = strconv.Atoi(os.Args[1])
	}
	u := make([]float64, n)
	v := make([]float64, n)
	tmp := make([]float64, n)
	for i := range u {
		u[i] = 1.0
	}
	for k := 0; k < 10; k++ {
		timesAtA(u, v, tmp)
		timesAtA(v, u, tmp)
	}
	var vbv, vv float64
	for i := 0; i < n; i++ {
		vbv += u[i] * v[i]
		vv += v[i] * v[i]
	}
	fmt.Printf("%.9f\n", math.Sqrt(vbv/vv))
}
