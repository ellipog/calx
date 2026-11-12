// Sorting: 5 million 64-bit integers and 5 million doubles read from binary files, and a million
// strings read a line each, each list sorted with the language's own sort and eleven evenly
// spaced elements printed.
fn main() {
    let dir = std::env::args().nth(1).expect("the input folder");
    let ib = std::fs::read(format!("{}/ints.bin", dir)).unwrap();
    let mut ints: Vec<i64> = ib.chunks_exact(8).map(|c| i64::from_le_bytes(c.try_into().unwrap())).collect();
    let fb = std::fs::read(format!("{}/floats.bin", dir)).unwrap();
    let mut floats: Vec<f64> = fb.chunks_exact(8).map(|c| f64::from_le_bytes(c.try_into().unwrap())).collect();
    let text = std::fs::read_to_string(format!("{}/strings.txt", dir)).unwrap();
    let mut strings: Vec<&str> = text.lines().collect();

    ints.sort_unstable();
    floats.sort_unstable_by(f64::total_cmp);
    strings.sort_unstable();

    for k in 0..11 {
        let i = (k * ints.len() / 10).min(ints.len() - 1);
        let f = (k * floats.len() / 10).min(floats.len() - 1);
        let s = (k * strings.len() / 10).min(strings.len() - 1);
        println!("{} {:.6} {}", ints[i], floats[f], strings[s]);
    }
}
