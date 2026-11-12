// Many files: 2,000 files of 1 to 16 KB written into 20 folders, read back and their bytes added
// up, then removed with their folders, through the language's own file functions.
use std::fs;

fn main() -> std::io::Result<()> {
    let mut args = std::env::args().skip(1);
    let root = format!("{}/run", args.next().expect("the scratch folder"));
    let files: usize = args.next().and_then(|s| s.parse().ok()).unwrap_or(2000);
    let pattern: Vec<u8> = (0..16384 + 251).map(|j| (j % 251) as u8).collect();
    fs::create_dir(&root)?;
    for d in 0..20 {
        fs::create_dir(format!("{}/d{}", root, d))?;
    }
    let mut written = 0;
    for i in 0..files {
        let size = 1024 + (i * 7919) % 15000;
        let from = i % 251;
        fs::write(format!("{}/d{}/f{}.bin", root, i % 20, i), &pattern[from..from + size])?;
        written += size;
    }
    let (mut sum, mut read) = (0u64, 0usize);
    for i in 0..files {
        let data = fs::read(format!("{}/d{}/f{}.bin", root, i % 20, i))?;
        read += data.len();
        sum += data.iter().map(|&b| b as u64).sum::<u64>();
    }
    for i in 0..files {
        fs::remove_file(format!("{}/d{}/f{}.bin", root, i % 20, i))?;
    }
    for d in 0..20 {
        fs::remove_dir(format!("{}/d{}", root, d))?;
    }
    fs::remove_dir(&root)?;
    println!("files {}\nwritten {}\nread {}\nsum {}", files, written, read, sum);
    Ok(())
}
