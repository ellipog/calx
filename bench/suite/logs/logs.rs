// Log analysis: a million lines of a web server's access log taken apart by splitting, no regular
// expressions, and summed: requests by status and by method, requests and bytes by path, with
// query strings dropped and numeric ids folded, and by hour, and the distinct client addresses.
use std::collections::{HashMap, HashSet};

fn is_number(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit())
}

fn main() {
    let path = std::env::args().nth(1).expect("the input's path");
    let text = std::fs::read_to_string(path).expect("the input");
    let mut lines = 0i64;
    let mut statuses: HashMap<i32, i64> = HashMap::new();
    let mut methods: HashMap<String, i64> = HashMap::new();
    let mut paths: HashMap<String, (i64, i64)> = HashMap::new();
    let mut hours = [(0i64, 0i64); 24];
    let mut ips: HashSet<String> = HashSet::new();
    for line in text.lines() {
        lines += 1;
        let (ip, rest) = line.split_once(' ').unwrap();
        if !ips.contains(ip) {
            ips.insert(ip.to_string());
        }
        let at = rest.find(':').unwrap();
        let hour: usize = rest[at + 1..at + 3].parse().unwrap();
        let (_, after) = rest.split_once('"').unwrap();
        let (request, tail) = after.split_once('"').unwrap();
        let parts: Vec<&str> = request.split(' ').collect();
        let mut path = parts[1];
        if let Some(q) = path.find('?') {
            path = &path[..q];
        }
        let path = match path.rfind('/') {
            Some(slash) if is_number(&path[slash + 1..]) => format!("{}{{id}}", &path[..slash + 1]),
            _ => path.to_string(),
        };
        let fields: Vec<&str> = tail.trim().split(' ').collect();
        let status: i32 = fields[0].parse().unwrap();
        let size: i64 = fields[1].parse().unwrap();
        *statuses.entry(status).or_insert(0) += 1;
        if let Some(n) = methods.get_mut(parts[0]) {
            *n += 1;
        } else {
            methods.insert(parts[0].to_string(), 1);
        }
        let p = paths.entry(path).or_insert((0, 0));
        p.0 += 1;
        p.1 += size;
        hours[hour].0 += 1;
        hours[hour].1 += size;
    }
    println!("lines {}\nips {}", lines, ips.len());
    let mut codes: Vec<_> = statuses.into_iter().collect();
    codes.sort();
    for (code, n) in codes {
        println!("status {} {}", code, n);
    }
    let mut names: Vec<_> = methods.into_iter().collect();
    names.sort();
    for (name, n) in names {
        println!("method {} {}", name, n);
    }
    let mut by_count: Vec<(String, i64, i64)> = paths.into_iter().map(|(p, (n, b))| (p, n, b)).collect();
    by_count.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(&b.0)));
    for (p, n, bytes) in by_count {
        println!("path {} {} {}", p, n, bytes);
    }
    for (h, (n, bytes)) in hours.iter().enumerate() {
        println!("hour {:02} {} {}", h, n, bytes);
    }
}
