// JSON: 200,000 player records read from a file, parsed into a tree, the tree written back as
// compact text, and the text read again into typed records, as a script reads and writes JSON.
// Calx uses std.json and `@json` structs, Rust serde_json's `Value` and derive, Go encoding/json,
// and Python json with dataclasses. C has no JSON library in its standard library.
package main

import (
	"encoding/json"
	"fmt"
	"os"
)

type Pos struct {
	X float64 `json:"x"`
	Z float64 `json:"z"`
}

type Player struct {
	ID     int64    `json:"id"`
	Name   string   `json:"name"`
	Pos    Pos      `json:"pos"`
	Tags   []string `json:"tags"`
	Active bool     `json:"active"`
	Level  int32    `json:"level"`
}

func main() {
	text, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	var tree any
	if err := json.Unmarshal(text, &tree); err != nil {
		panic(err)
	}
	written, err := json.Marshal(tree)
	if err != nil {
		panic(err)
	}
	var players []Player
	if err := json.Unmarshal(text, &players); err != nil {
		panic(err)
	}
	var active, levels int64
	var x float64
	tags := 0
	for _, p := range players {
		if p.Active {
			active++
		}
		levels += int64(p.Level)
		x += p.Pos.X
		tags += len(p.Tags)
	}
	fmt.Printf("records %d %d\n", len(tree.([]any)), len(players))
	fmt.Printf("active %d\nlevels %d\nx %.2f\ntags %d\n", active, levels, x, tags)
	fmt.Printf("written %d\n", len(written))
}
