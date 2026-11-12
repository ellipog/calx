// JSON: 200,000 player records read from a file, parsed into a tree, the tree written back as
// compact text, and the text read again into typed records, as a script reads and writes JSON.
// Calx uses std.json and `@json` structs, Rust serde_json's `Value` and derive, Go encoding/json,
// and Python json with dataclasses. C has no JSON library in its standard library.
use serde::Deserialize;

#[derive(Deserialize)]
struct Pos {
    x: f64,
    #[allow(dead_code)]
    z: f64,
}

#[derive(Deserialize)]
struct Player {
    #[allow(dead_code)]
    id: i64,
    #[allow(dead_code)]
    name: String,
    pos: Pos,
    tags: Vec<String>,
    active: bool,
    level: i32,
}

fn main() {
    let path = std::env::args().nth(1).expect("the input's path");
    let text = std::fs::read_to_string(path).expect("the input");
    let tree: serde_json::Value = serde_json::from_str(&text).unwrap();
    let written = serde_json::to_string(&tree).unwrap();
    let players: Vec<Player> = serde_json::from_str(&text).unwrap();
    let (mut active, mut levels, mut x, mut tags) = (0i64, 0i64, 0.0f64, 0usize);
    for p in &players {
        if p.active {
            active += 1;
        }
        levels += p.level as i64;
        x += p.pos.x;
        tags += p.tags.len();
    }
    println!("records {} {}", tree.as_array().unwrap().len(), players.len());
    println!("active {}\nlevels {}\nx {:.2}\ntags {}", active, levels, x, tags);
    println!("written {}", written.len());
}
