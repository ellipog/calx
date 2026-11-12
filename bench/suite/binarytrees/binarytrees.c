// binary-trees: the Benchmarks Game's allocation test. Perfect binary trees are made node by node
// on the heap, counted and freed: a stretch tree, a long-lived one, and many short-lived ones of
// each depth. Every node is its own allocation, as each language allocates one.
#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
    struct Node *left, *right;
} Node;

static Node *make(int depth) {
    Node *n = malloc(sizeof(Node));
    if (depth > 0) {
        n->left = make(depth - 1);
        n->right = make(depth - 1);
    } else {
        n->left = n->right = NULL;
    }
    return n;
}

static int check(const Node *n) {
    int c = 1;
    if (n->left) c += check(n->left);
    if (n->right) c += check(n->right);
    return c;
}

static void drop(Node *n) {
    if (n->left) drop(n->left);
    if (n->right) drop(n->right);
    free(n);
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 10;
    int min_depth = 4;
    int max_depth = min_depth + 2 > n ? min_depth + 2 : n;
    int stretch = max_depth + 1;
    Node *s = make(stretch);
    printf("stretch tree of depth %d\t check: %d\n", stretch, check(s));
    drop(s);
    Node *long_lived = make(max_depth);
    for (int d = min_depth; d <= max_depth; d += 2) {
        int iterations = 1 << (max_depth - d + min_depth);
        int total = 0;
        for (int i = 0; i < iterations; i++) {
            Node *t = make(d);
            total += check(t);
            drop(t);
        }
        printf("%d\t trees of depth %d\t check: %d\n", iterations, d, total);
    }
    printf("long lived tree of depth %d\t check: %d\n", max_depth, check(long_lived));
    drop(long_lived);
    return 0;
}
