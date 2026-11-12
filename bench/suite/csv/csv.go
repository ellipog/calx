// CSV report: a million rows of sales read with the language's CSV reader, some fields quoted with
// commas and doubled quotes inside, and summed by region and product and by month: the rows, the
// quantity and the revenue, in the order of the file. Calx uses std.csv, Rust the csv crate, Go
// encoding/csv and Python csv; C has none, so it reads the fields by hand.
package main

import (
	"bufio"
	"encoding/csv"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
)

type Group struct {
	rows, quantity int64
	revenue        float64
}

func main() {
	f, err := os.Open(os.Args[1])
	if err != nil {
		panic(err)
	}
	defer f.Close()
	reader := csv.NewReader(bufio.NewReader(f))
	// One row, read into again for each row, as Calx's reader reads into one list.
	reader.ReuseRecord = true
	if _, err := reader.Read(); err != nil {
		panic(err)
	}
	groups := make(map[string]*Group)
	var months [13]float64
	for {
		row, err := reader.Read()
		if err == io.EOF {
			break
		}
		if err != nil {
			panic(err)
		}
		quantity, _ := strconv.ParseInt(row[3], 10, 64)
		price, _ := strconv.ParseFloat(row[4], 64)
		revenue := float64(quantity) * price
		key := row[1] + "|" + row[2]
		g := groups[key]
		if g == nil {
			g = &Group{}
			groups[key] = g
		}
		g.rows++
		g.quantity += quantity
		g.revenue += revenue
		month, _ := strconv.Atoi(row[0][5:7])
		months[month] += revenue
	}
	keys := make([]string, 0, len(groups))
	for k := range groups {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		g := groups[k]
		fmt.Printf("%s|%d|%d|%.2f\n", k, g.rows, g.quantity, g.revenue)
	}
	for m := 1; m <= 12; m++ {
		fmt.Printf("month %d %.2f\n", m, months[m])
	}
}
