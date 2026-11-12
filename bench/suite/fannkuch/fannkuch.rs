// fannkuch-redux: the Benchmarks Game's pancake flips over every permutation of 1..n, in the order
// its counting algorithm makes them, with the checksum and the most flips any permutation took.
fn main() {
    let n: usize = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(7);
    let mut perm = vec![0usize; n];
    let mut perm1: Vec<usize> = (0..n).collect();
    let mut count = vec![0usize; n];
    let mut max_flips = 0i32;
    let mut checksum = 0i32;
    let mut perm_count = 0i32;
    let mut r = n;
    loop {
        while r != 1 {
            count[r - 1] = r;
            r -= 1;
        }
        perm.copy_from_slice(&perm1);
        let mut flips = 0i32;
        loop {
            let k = perm[0];
            if k == 0 {
                break;
            }
            perm[..=k].reverse();
            flips += 1;
        }
        max_flips = max_flips.max(flips);
        checksum += if perm_count % 2 == 0 { flips } else { -flips };
        loop {
            if r == n {
                println!("{}\nPfannkuchen({}) = {}", checksum, n, max_flips);
                return;
            }
            let perm0 = perm1[0];
            for i in 0..r {
                perm1[i] = perm1[i + 1];
            }
            perm1[r] = perm0;
            count[r] -= 1;
            if count[r] > 0 {
                break;
            }
            r += 1;
        }
        perm_count += 1;
    }
}
