// binary-trees: the Benchmarks Game's allocation test. Perfect binary trees are made node by node
// on the heap, counted and freed: a stretch tree, a long-lived one, and many short-lived ones of
// each depth. Every node is its own allocation, as each language allocates one; Go's collector
// frees them.
package main

import (
	"fmt"
	"os"
	"strconv"
)

type Node struct {
	left, right *Node
}

func newTree(depth int) *Node {
	if depth <= 0 {
		return &Node{}
	}
	return &Node{newTree(depth - 1), newTree(depth - 1)}
}

func (n *Node) check() int {
	c := 1
	if n.left != nil {
		c += n.left.check()
	}
	if n.right != nil {
		c += n.right.check()
	}
	return c
}

func main() {
	n := 10
	if len(os.Args) > 1 {
		n, _ = strconv.Atoi(os.Args[1])
	}
	minDepth := 4
	maxDepth := max(minDepth+2, n)
	stretch := maxDepth + 1
	fmt.Printf("stretch tree of depth %d\t check: %d\n", stretch, newTree(stretch).check())
	longLived := newTree(maxDepth)
	for d := minDepth; d <= maxDepth; d += 2 {
		iterations := 1 << (maxDepth - d + minDepth)
		total := 0
		for i := 0; i < iterations; i++ {
			total += newTree(d).check()
		}
		fmt.Printf("%d\t trees of depth %d\t check: %d\n", iterations, d, total)
	}
	fmt.Printf("long lived tree of depth %d\t check: %d\n", maxDepth, longLived.check())
}
