// bench/json/json.calx with serde_json, as Rust code usually reads and writes JSON: a `Value` tree,
// `to_string`, and structs with `#[derive(Deserialize)]`, which serde reads straight from the text.
use serde::Deserialize;
use std::time::Instant;

#[derive(Deserialize)]
#[allow(dead_code)]
struct Pos {
    x: f64,
    z: f64,
}

#[derive(Deserialize)]
#[allow(dead_code)]
struct Player {
    id: i64,
    name: String,
    pos: Pos,
    tags: Vec<String>,
    active: bool,
    level: i32,
}

fn main() {
    let mut text = String::from("[");
    for i in 0..100000u64 {
        if i > 0 {
            text.push(',');
        }
        let active = if i % 2 == 0 { "true" } else { "false" };
        text.push_str(&format!(
            "{{\"id\":{i},\"name\":\"player {i}\",\"pos\":{{\"x\":{i}.5,\"z\":-{i}.25}},\"tags\":[\"builder\",\"mod\\u00e9rateur\"],\"active\":{active},\"level\":{}}}",
            i % 100
        ));
    }
    text.push(']');
    let (mut parse, mut write, mut typed) = (u128::MAX, u128::MAX, u128::MAX);
    let mut total = 0usize;
    for _ in 0..7 {
        let t0 = Instant::now();
        let tree: serde_json::Value = serde_json::from_str(&text).unwrap();
        let t1 = Instant::now();
        let out = serde_json::to_string(&tree).unwrap();
        let t2 = Instant::now();
        let players: Vec<Player> = serde_json::from_str(&text).unwrap();
        let t3 = Instant::now();
        total += tree.as_array().unwrap().len() + out.len() + players.len() + players[99999].level as usize;
        parse = parse.min((t1 - t0).as_micros());
        write = write.min((t2 - t1).as_micros());
        typed = typed.min((t3 - t2).as_micros());
    }
    println!("rust: {} bytes, parse {} us, write {} us, typed {} us, total {}", text.len(), parse, write, typed, total);
}
