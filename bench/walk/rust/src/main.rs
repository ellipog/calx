// bench/walk/walk.calx with the walkdir and glob crates, as Rust code usually walks a tree: the
// names sorted, as std.fs sorts them, and the folder itself left out, as `walk` leaves it out.
use std::time::Instant;

fn main() {
    let root = std::env::args().nth(1).expect("a folder");
    if !std::path::Path::new(&root).join("d19").join("e19").is_dir() {
        eprintln!("make the tree with walk.calx first");
        std::process::exit(2);
    }
    let (mut walked, mut globbed) = (u128::MAX, u128::MAX);
    let mut total = 0usize;
    for _ in 0..7 {
        let t0 = Instant::now();
        let (mut files, mut dirs, mut chars) = (0usize, 0usize, 0usize);
        for e in walkdir::WalkDir::new(&root).min_depth(1).sort_by_file_name() {
            let e = e.unwrap();
            let t = e.file_type();
            if t.is_file() {
                files += 1;
            } else if t.is_dir() {
                dirs += 1;
            }
            chars += e.path().as_os_str().len();
        }
        let t1 = Instant::now();
        let mut found = 0usize;
        for p in glob::glob(&format!("{}/**/*.calx", root)).unwrap() {
            found += 1;
            chars += p.unwrap().as_os_str().len();
        }
        let t2 = Instant::now();
        total += files + dirs + found + chars;
        walked = walked.min((t1 - t0).as_micros());
        globbed = globbed.min((t2 - t1).as_micros());
    }
    println!("rust: walk {} us, glob {} us, total {}", walked, globbed, total);
}
