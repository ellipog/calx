# JSON: 200,000 player records read from a file, parsed into a tree, the tree written back as
# compact text, and the text read again into typed records, as a script reads and writes JSON.
# Calx uses std.json and `@json` structs, Rust serde_json's `Value` and derive, Go encoding/json,
# and Python json with dataclasses. C has no JSON library in its standard library.
import dataclasses
import json
import sys


@dataclasses.dataclass
class Pos:
    x: float
    z: float


@dataclasses.dataclass
class Player:
    id: int
    name: str
    pos: Pos
    tags: list
    active: bool
    level: int


def main():
    with open(sys.argv[1], encoding="utf-8") as f:
        text = f.read()
    tree = json.loads(text)
    written = json.dumps(tree, separators=(",", ":"), ensure_ascii=False)
    players = [Player(d["id"], d["name"], Pos(**d["pos"]), d["tags"], d["active"], d["level"]) for d in json.loads(text)]
    active = levels = tags = 0
    x = 0.0
    for p in players:
        if p.active:
            active += 1
        levels += p.level
        x += p.pos.x
        tags += len(p.tags)
    print(f"records {len(tree)} {len(players)}")
    print(f"active {active}\nlevels {levels}\nx {x:.2f}\ntags {tags}")
    print(f"written {len(written.encode())}")


main()
