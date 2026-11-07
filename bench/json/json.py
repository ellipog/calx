"""bench/json/json.calx with Python's json module, whose parser and writer are C: a tree of dicts and
lists, json.dumps, and dataclasses built from the dicts, as a script makes typed records."""
import dataclasses
import json
import time


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
    parts = []
    for i in range(100000):
        active = "true" if i % 2 == 0 else "false"
        parts.append('{"id":%d,"name":"player %d","pos":{"x":%d.5,"z":-%d.25},"tags":["builder","mod\\u00e9rateur"],"active":%s,"level":%d}'
                     % (i, i, i, i, active, i % 100))
    text = "[" + ",".join(parts) + "]"
    parse = write = typed = float("inf")
    total = 0
    for _ in range(7):
        t0 = time.perf_counter()
        tree = json.loads(text)
        t1 = time.perf_counter()
        out = json.dumps(tree, separators=(",", ":"), ensure_ascii=False)
        t2 = time.perf_counter()
        players = []
        for d in json.loads(text):
            players.append(Player(d["id"], d["name"], Pos(d["pos"]["x"], d["pos"]["z"]), d["tags"], d["active"], d["level"]))
        t3 = time.perf_counter()
        total += len(tree) + len(out.encode()) + len(players) + players[99999].level
        parse = min(parse, t1 - t0)
        write = min(write, t2 - t1)
        typed = min(typed, t3 - t2)
    print("python: %d bytes, parse %d us, write %d us, typed %d us, total %d"
          % (len(text), parse * 1e6, write * 1e6, typed * 1e6, total))


main()
