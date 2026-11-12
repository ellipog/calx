// pidigits: the Benchmarks Game's digits of π from the unbounded spigot, ten to a line, with
// integers of any size. Each language uses its own big integers: std.bigint in Calx, num-bigint
// in Rust, math/big here and Python's int. C has none in its standard library.
package main

import (
	"bufio"
	"fmt"
	"math/big"
	"os"
	"strconv"
)

func main() {
	n := 27
	if len(os.Args) > 1 {
		n, _ = strconv.Atoi(os.Args[1])
	}
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	numer, accum, denom := big.NewInt(1), big.NewInt(0), big.NewInt(1)
	d, d4, tmp := new(big.Int), new(big.Int), new(big.Int)
	line := make([]byte, 0, 10)
	i, k := 0, 0
	for i < n {
		k++
		k2 := big.NewInt(int64(k*2 + 1))
		accum.Add(accum, tmp.Lsh(numer, 1))
		accum.Mul(accum, k2)
		denom.Mul(denom, k2)
		numer.Mul(numer, big.NewInt(int64(k)))
		if numer.Cmp(accum) > 0 {
			continue
		}
		d.Quo(tmp.Add(tmp.Mul(numer, big.NewInt(3)), accum), denom)
		d4.Quo(tmp.Add(tmp.Mul(numer, big.NewInt(4)), accum), denom)
		if d.Cmp(d4) != 0 {
			continue
		}
		digit := d.Int64()
		line = append(line, byte('0'+digit))
		i++
		if i%10 == 0 {
			fmt.Fprintf(out, "%s\t:%d\n", line, i)
			line = line[:0]
		}
		accum.Sub(accum, tmp.Mul(denom, big.NewInt(digit)))
		accum.Mul(accum, big.NewInt(10))
		numer.Mul(numer, big.NewInt(10))
	}
	if len(line) > 0 {
		fmt.Fprintf(out, "%-10s\t:%d\n", line, n)
	}
}
