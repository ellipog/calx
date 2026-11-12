// binary-trees: the Benchmarks Game's allocation test. Perfect binary trees are made node by node
// on the heap, counted and freed: a stretch tree, a long-lived one, and many short-lived ones of
// each depth. Every node is its own allocation, as each language allocates one.
struct Node {
    left: Option<Box<Node>>,
    right: Option<Box<Node>>,
}

fn make(depth: i32) -> Node {
    if depth <= 0 {
        return Node { left: None, right: None };
    }
    Node { left: Some(Box::new(make(depth - 1))), right: Some(Box::new(make(depth - 1))) }
}

fn check(n: &Node) -> i32 {
    let mut c = 1;
    if let Some(l) = &n.left {
        c += check(l);
    }
    if let Some(r) = &n.right {
        c += check(r);
    }
    c
}

fn main() {
    let n: i32 = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(10);
    let min_depth = 4;
    let max_depth = (min_depth + 2).max(n);
    let stretch = max_depth + 1;
    println!("stretch tree of depth {}\t check: {}", stretch, check(&make(stretch)));
    let long_lived = make(max_depth);
    let mut d = min_depth;
    while d <= max_depth {
        let iterations = 1 << (max_depth - d + min_depth);
        let mut total = 0;
        for _ in 0..iterations {
            total += check(&make(d));
        }
        println!("{}\t trees of depth {}\t check: {}", iterations, d, total);
        d += 2;
    }
    println!("long lived tree of depth {}\t check: {}", max_depth, check(&long_lived));
}
