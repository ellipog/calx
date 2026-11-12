// Log analysis: a million lines of a web server's access log taken apart by splitting, no regular
// expressions, and summed: requests by status and by method, requests and bytes by path, with
// query strings dropped and numeric ids folded, and by hour, and the distinct client addresses.
package main

import (
	"bufio"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
)

func isNumber(s string) bool {
	if s == "" {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return true
}

type pathCount struct {
	path      string
	n, bytes  int64
}

func main() {
	data, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	text := string(data)
	var lines int64
	statuses := make(map[int]int64)
	methods := make(map[string]int64)
	paths := make(map[string]*pathCount)
	var hours [24][2]int64
	ips := make(map[string]struct{})
	for len(text) > 0 {
		line, rest0, _ := strings.Cut(text, "\n")
		text = rest0
		lines++
		ip, rest, _ := strings.Cut(line, " ")
		ips[ip] = struct{}{}
		at := strings.IndexByte(rest, ':')
		hour, _ := strconv.Atoi(rest[at+1 : at+3])
		_, after, _ := strings.Cut(rest, "\"")
		request, tail, _ := strings.Cut(after, "\"")
		parts := strings.Split(request, " ")
		path := parts[1]
		if q := strings.IndexByte(path, '?'); q >= 0 {
			path = path[:q]
		}
		if slash := strings.LastIndexByte(path, '/'); slash >= 0 && isNumber(path[slash+1:]) {
			path = path[:slash+1] + "{id}"
		}
		fields := strings.Split(strings.TrimSpace(tail), " ")
		status, _ := strconv.Atoi(fields[0])
		size, _ := strconv.ParseInt(fields[1], 10, 64)
		statuses[status]++
		methods[parts[0]]++
		p := paths[path]
		if p == nil {
			p = &pathCount{path: path}
			paths[path] = p
		}
		p.n++
		p.bytes += size
		hours[hour][0]++
		hours[hour][1] += size
	}
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	fmt.Fprintf(out, "lines %d\nips %d\n", lines, len(ips))
	codes := make([]int, 0, len(statuses))
	for code := range statuses {
		codes = append(codes, code)
	}
	sort.Ints(codes)
	for _, code := range codes {
		fmt.Fprintf(out, "status %d %d\n", code, statuses[code])
	}
	names := make([]string, 0, len(methods))
	for name := range methods {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		fmt.Fprintf(out, "method %s %d\n", name, methods[name])
	}
	byCount := make([]*pathCount, 0, len(paths))
	for _, p := range paths {
		byCount = append(byCount, p)
	}
	sort.Slice(byCount, func(i, j int) bool {
		if byCount[i].n != byCount[j].n {
			return byCount[i].n > byCount[j].n
		}
		return byCount[i].path < byCount[j].path
	})
	for _, p := range byCount {
		fmt.Fprintf(out, "path %s %d %d\n", p.path, p.n, p.bytes)
	}
	for h := 0; h < 24; h++ {
		fmt.Fprintf(out, "hour %02d %d %d\n", h, hours[h][0], hours[h][1])
	}
}
