// Many small objects: 20 rounds of 200,000 records, each with an id, a name made with formatting
// and a list of 1 to 8 numbers, three allocations a record. Each round's records are summed, one
// in 16 kept for the rest of the run, and the others left to the collector.
package main

import (
	"fmt"
	"os"
	"strconv"
)

type Record struct {
	id     int64
	name   string
	values []int64
}

func main() {
	rounds, _ := strconv.ParseInt(os.Args[1], 10, 64)
	per, _ := strconv.ParseInt(os.Args[2], 10, 64)
	var kept []*Record
	var total int64
	for round := int64(0); round < rounds; round++ {
		var batch []*Record
		for i := int64(0); i < per; i++ {
			id := round*per + i
			var values []int64
			for k := int64(1); k < 2+id%8; k++ {
				values = append(values, id*k)
			}
			batch = append(batch, &Record{id, fmt.Sprintf("item-%d", id), values})
		}
		for _, r := range batch {
			for _, v := range r.values {
				total += v
			}
			total += int64(len(r.name))
		}
		for _, r := range batch {
			if r.id%16 == 0 {
				kept = append(kept, r)
			}
		}
	}
	names := 0
	var values int64
	for _, r := range kept {
		names += len(r.name)
		for _, v := range r.values {
			values += v
		}
	}
	fmt.Printf("total %d\nkept %d %d %d\n", total, len(kept), names, values)
}
