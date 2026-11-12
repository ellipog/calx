# binary-trees: the Benchmarks Game's allocation test. Perfect binary trees are made node by node
# on the heap, counted and freed: a stretch tree, a long-lived one, and many short-lived ones of
# each depth. Every node is its own allocation, as each language allocates one.
import sys


class Node:
    __slots__ = ("left", "right")

    def __init__(self, left, right):
        self.left = left
        self.right = right


def make(depth):
    if depth <= 0:
        return Node(None, None)
    return Node(make(depth - 1), make(depth - 1))


def check(n):
    c = 1
    if n.left is not None:
        c += check(n.left)
    if n.right is not None:
        c += check(n.right)
    return c


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    min_depth = 4
    max_depth = max(min_depth + 2, n)
    stretch = max_depth + 1
    print(f"stretch tree of depth {stretch}\t check: {check(make(stretch))}")
    long_lived = make(max_depth)
    for d in range(min_depth, max_depth + 1, 2):
        iterations = 1 << (max_depth - d + min_depth)
        total = 0
        for _ in range(iterations):
            total += check(make(d))
        print(f"{iterations}\t trees of depth {d}\t check: {total}")
    print(f"long lived tree of depth {max_depth}\t check: {check(long_lived)}")


main()
