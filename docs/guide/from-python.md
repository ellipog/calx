---
title: Coming from Python
description: The Python a script reaches for and the Calx that does the same - running a script, text, lists and dicts, files, other programs, JSON, CSV, URLs, hashes, dates, the web, and where Calx differs on purpose.
---

# Coming from Python

Calx is meant for the scripts you'd write in Python too. A script is one file, run with `calx run script.calx`. There's no virtual environment, nothing to install with pip, and no interpreter version to match. The standard library has what scripts reach for, often with Python's own behaviour: `std.csv` reads CSV as `csv.reader` does, and `std.stats` gives what `statistics` gives, to the last bit. The difference is that the checker reads the whole script before it runs, and the script then runs as native code.

This page puts the Python you know next to the Calx that does the same. It assumes you've read [Getting started](getting-started.md).

## Running a script

| Python | Calx |
| --- | --- |
| `python tool.py a b` | `calx tool.calx a b`, or `calx run tool.calx -- a b` |
| `uv init tool` | `calx new tool`, a folder with `tool.calx` and a `.gitignore`, or `calx new tool.calx` for one script ([starting a program](../reference/command.md#starting-a-program)) |
| `python -c "print(2 ** 20)"` | `calx eval "1 << 20"`, which prints the last statement's value ([running code](../reference/command.md#running-code-from-the-command-line)) |
| `#!/usr/bin/env python3` | `#!/usr/bin/env calx` ([running a file](../reference/command.md#running-a-file)) |
| `if __name__ == "__main__":` | `fn main()`, where the program starts |
| `sys.argv[1:]` | `args()` from [`std.env`](../std/env.md), without the program's name |
| `argparse` | `@cli` on a struct, from [`std.cli`](../std/cli.md), with `--help` written from its docs |
| `sys.exit(2)` | `exit(2)` from [`std.process`](../std/process.md), which returns `never`, so it can end a function that returns a value |
| `sys.exit("bad input")` | A function of your own that prints it and calls `exit`, declared `-> never` ([functions that never return](../reference/functions.md#functions-that-never-return)) |
| `os.environ.get("HOME")` | `variable("HOME")` from [`std.env`](../std/env.md), a `String?` |
| `input()`, `for line in sys.stdin:` | `Lines()` from [`std.io`](../std/io.md), whose `next()` gives each line, then `none` |
| `print(s)`, `print(s, end="")`, `print(s, file=sys.stderr)` | `println(s)`, `print(s)`, `eprintln(s)` ([printing](../reference/printing.md)) |
| `black`, `ruff format` | `calx fmt`, one format with no settings ([formatting](../reference/command.md#formatting)) |
| `unittest`, `pytest` | `@test` on a function, and `calx test tool.calx`, which runs each test in a process of its own ([testing](../reference/command.md#testing)) |
| `assert x == 1, "why"` | `assert(x == 1, "why")` |
| `pip install requests` | Nothing: `std.http` comes with Calx |

The first run builds the script. Later runs reuse that build until the file changes, so the script starts in milliseconds. `calx run --release` builds it optimized, for scripts that do a lot of work.

## A script, both ways

Count the words on standard input and print the three most common:

```python
import sys
from collections import Counter

counts = Counter()
for line in sys.stdin:
    counts.update(line.split())
for word, n in counts.most_common(3):
    print(f"{word}: {n}")
```

```calx
import std.io.{Lines}

fn main() {
    var counts = Map<String, i32>()
    var input = Lines()
    while let line = input.next() {
        for word in line.split_whitespace() {
            if counts.contains(word) {
                counts[word] += 1
            } else {
                counts.insert(word.copy(), 1)
            }
        }
    }
    var top: List<(String, i32)> = []
    for (word, n) in counts {
        top.push((word.copy(), n))
    }
    top.sort()
    top.sort_by(|a, b| b.1.cmp(a.1))
    for (word, n) in top[..3usize.min(top.len())] {
        println("{word}: {n}")
    }
}
// stdin: stone dirt stone
// stdin: sand stone dirt gravel
// out: stone: 3
// out: dirt: 2
// out: gravel: 1
```

- `while let` runs once for each line until the input ends, when `input.next()` gives `none`.
- `word.copy()` is there because the map keeps its own copy of the key, and the loop's `word` is only a view of part of `line`. Copies are always written out, so you can see where memory goes.
- A `Map` keeps its keys in no particular order. So the list is sorted by word first, and then by count, highest first. `sort_by` is stable, which keeps words with the same count in order, so the output is the same on every run. Python's `most_common` keeps ties in the order the words first appeared.

## When something fails

Python raises an exception, and the script stops with a traceback. In Calx, a function that can fail says so in its type, and `try` passes the failure up to the caller:

```python
path = "settings.txt"
text = open(path).read()  # FileNotFoundError, with a traceback
```

```calx
import std.fs.{read_text}

fn main() ! Error {
    let path = "settings.txt"
    let text = try read_text(path).context("reading {path}") ?? String()
    println(text)
}
// stderr: error: reading settings.txt: not found
// status: 1
```

- `fn main() ! Error` says `main` can fail. When it does, the script writes `error: ` and what went wrong, then exits with status 1.
- `.context(...)` adds what was being done, the way a good traceback reads.
- `read_text` gives a `String?`. It's `none` when the file isn't UTF-8, and `?? String()` turns that into empty text. Python would raise `UnicodeDecodeError` instead.
- `match` handles a failure where it happens, as `try`/`except` does in Python ([errors](../reference/errors.md)).

## Text

| Python | Calx |
| --- | --- |
| `f"{name} has {n}"`, `f"{x:.2f}"` | `"{name} has {n}"`, `"{x:.2}"`: every string literal interpolates ([printing](../reference/printing.md)). Write `\{` for a brace, or use a raw string `#"..."#` |
| `len(s)` | `s.len()` in bytes of UTF-8, or count the runes with `for r in s.runes()` |
| `s.split(",")`, `s.split()` | `s.split(",")`, `s.split_whitespace()` |
| `s.strip()`, `s.removeprefix("x")` | `s.trim()`, `s.trim_prefix("x")` |
| `s.splitlines()` | `s.lines()` |
| `s.partition("=")` | `s.split_once("=")`, a `(String, String)?` |
| `", ".join(xs)` | `xs.join(", ")` |
| `x in s`, `s.find(t)` | `s.contains(t)`, `s.find(t)`, a `usize?` rather than `-1` |
| `s.lower()` | `s.to_lower_root()` |
| `int(s)`, `float(s)` | `i32.parse(s)`, `f64.parse(s)`, which give `none` for text that isn't one |
| `re.sub(p, r, s)` | `Regex.new(p)` from [`std.regex`](../std/regex.md), then `replace_all(s, r)`. Its syntax is Rust's, close to Python's |
| `s.encode()`, `b.decode()` | `s.to_bytes()`, `String.from_utf8(b)`, which gives `none` for bad UTF-8, or [`decode_lossy_unicode`](../std/text-utf8.md) for Python's `errors="replace"` |

## Lists and dicts

| Python | Calx |
| --- | --- |
| `xs = [1, 2]`, `xs.append(3)` | `var xs = [1, 2]`, `xs.push(3)`. A `let` can't change, and a `var` can |
| `xs[-1]` | `xs[xs.len() - 1]` |
| `xs[1:3]` | `xs[1..3]` |
| `[f(x) for x in xs if p(x)]` | `xs.filter(\|x\| p(x)).map(\|x\| f(x))` ([closures](../reference/closures.md)) |
| `sum(xs)`, `min(xs)`, `max(xs)` | `xs.sum()`, `xs.min()`, `xs.max()`, the last two `none` for an empty list ([lists](../reference/lists.md#sum-min-and-max)) |
| `xs.sort()`, `xs.sort(key=f)` | `xs.sort()`, `xs.sort_by_key(\|x\| f(x))` for a key that's `Copy`, such as a number, or `xs.sort_by(\|a, b\| ...)`. All sort in place, and the last two are stable |
| `any(...)`, `all(...)` | `xs.any(\|x\| ...)`, `xs.all(\|x\| ...)` |
| `enumerate(xs)` | `for (i, x) in xs.enumerate()` |
| `zip(xs, ys)` | `for (x, y) in xs.zip(ys)` |
| `dict`, which keeps the order keys went in | `LinkedMap<K, V>` ([linked maps](../reference/linked-maps.md)). A `Map` is faster where the order doesn't matter |
| `d.get(k, 0)` | `d.get(k) ?? 0` |
| `set()` | `Set<T>`, or `LinkedSet<T>` to keep the order |
| `collections.deque` | `Deque<T>` ([deques](../reference/deques.md)) |
| `heapq` | `PriorityQueue<T>` ([priority queues](../reference/priority-queues.md)) |
| `None` | `none`, in a type that says it may be missing: `String?` ([optionals](../reference/optionals.md)) |

## Files and programs

| Python | Calx, from [`std.fs`](../std/fs.md), [`std.path`](../std/path.md) and [`std.process`](../std/process.md) |
| --- | --- |
| `open(p).read()` | `read_text(p)`, or `read(p)` for bytes |
| `open(p, "w").write(t)` | `write_text(p, t)`, or `write(p, bytes)` |
| `for line in open(p):` | `for line in (try read_text(p) ?? String()).lines()` |
| `os.listdir(d)`, `glob.glob("**/*.py", recursive=True)` | `list(d)`, `glob("**/*.py")`, sorted |
| `os.walk(d)` | `walk(d)` |
| `os.makedirs(d, exist_ok=True)` | `make_dir_all(d)` |
| `shutil.copy`, `shutil.rmtree` | `copy`, `remove_dir_all` |
| `os.path.join`, `pathlib.Path(p).suffix` | `path.join`, `path.file_extension` |
| `tempfile.mkdtemp()` | `temp_dir()` |
| `subprocess.run(["git", "status"], check=True)` | `Command.new("git").arg("status").run()` |
| `subprocess.check_output(...)` | `Command.new(...).read()`, its output as text |
| `subprocess.run(..., capture_output=True)` | `Command.new(...).output()`, with the status and both streams |
| `os.wait4(pid, 0)`, `psutil.Process(pid).memory_info()` | `unsafe { status.usage() }`, `unsafe { child.usage() }`: CPU time and memory, the same on both systems, without `ctypes` |

```calx
import std.fs.{temp_dir, write_text, make_dir_all, glob, remove_dir_all}
import std.path

fn main() ! Error {
    let dir = try temp_dir()
    try make_dir_all(path.join(dir, "logs"))
    for name in ["a.log", "b.log", "notes.txt"] {
        try write_text(path.join(dir, "logs/{name}"), "hello\n")
    }
    for p in try glob(path.join(dir, "logs/*.log")) {
        println(path.file_name(p) ?? "")
    }
    // out: a.log
    // out: b.log
    try remove_dir_all(dir)
}
```

## Data, the web, hashes and time

| Python | Calx |
| --- | --- |
| `json.loads(t)`, `json.dumps(v, indent=2)` | `parse(t)`, `pretty(v, 2)` from [`std.json`](../std/json.md). `@json` on a struct reads and writes it from its fields' types, as a dataclass with pydantic would |
| `csv.reader(f)`, `csv.DictReader(f)`, `csv.writer(f)` | `parse`, `parse_records` and `format` from [`std.csv`](../std/csv.md), or `Reader` for a row at a time |
| `urllib.parse.quote`, `urlsplit`, `urljoin` | `encode`, `Url.parse`, `url.join` from [`std.url`](../std/url.md) |
| `uuid.uuid4()` | `Uuid.random()` from [`std.uuid`](../std/uuid.md) |
| `requests.get(url).text` | `fetch(url)` from [`std.http`](../std/http.md), then `String.from_utf8(resp.body)`, ready for `parse` |
| `hashlib.sha256(b).hexdigest()` | `hex(sha256(b))`, with [`std.hash`](../std/hash.md) and [`std.bytes`](../std/bytes.md) |
| `base64.b64encode(b)` | `base64(b)` from [`std.bytes`](../std/bytes.md) |
| `zipfile`, `tarfile`, `gzip` | [`std.zip`](../std/zip.md), [`std.tar`](../std/tar.md), [`std.zlib`](../std/zlib.md) |
| `datetime.now()`, `strftime`, `strptime` | `unsafe { Timestamp.now() }`, `format` and `parse` from [`std.time`](../std/time.md), with Python's codes |
| `time.perf_counter()` | `unsafe { Instant.now() }` from [`std.time`](../std/time.md) |
| `time.sleep(0.5)` | `unsafe { sleep(500_000_000) }`, in nanoseconds, from [`std.thread`](../std/thread.md) |
| `random.seed(1)`, `random.choice(xs)` | `var r = Random.new(1)`, `r.pick(xs)` from [`std.random`](../std/random.md), the same numbers from a seed on every system. `Random.system()` differs each run |
| `statistics.median(xs)` | `median(xs)` from [`std.stats`](../std/stats.md), with Python's results to the last bit |
| `decimal.Decimal("0.1")` | `Decimal.parse("0.1")` from [`std.decimal`](../std/decimal.md) |
| `difflib.unified_diff` | [`std.diff`](../std/diff.md) |

```calx
import std.csv.{parse_records}
import std.json.{json}
import std.hash.{sha256}
import std.bytes.{hex}

@json
struct Summary {
    mobs: usize
    total_hp: i32
    sha256: String
}

fn main() {
    let text = "name,hp\nzombie,20\nwitch,26\n"
    let mobs = parse_records(text)
    var total = 0
    for mob in mobs {
        total += i32.parse(mob["hp"]) ?? 0
    }
    let s = Summary { mobs: mobs.len(), total_hp: total, sha256: hex(sha256(text.to_bytes()))[..16] }
    println(s.to_json_text())    // out: {"mobs":2,"total_hp":46,"sha256":"2be3b03621cd1c2e"}
}
```

Reading a clock or sleeping takes `unsafe`. Safe Calx can't see time, so its results never depend on how fast it ran. Code that reads the clock says so where it does ([why](../std/time.md#why-they-need-unsafe)).

## Where Calx differs on purpose

- **Types are checked before the script runs.** A typo in a name, a missing argument or a `String` where a number was wanted is an error with the line and column, not a traceback an hour into a run. Most types are worked out for you, so a script reads much like Python. Function parameters and the elements of an empty list say their type.
- **Nothing is `None` unless its type says so.** A value that may be missing is an optional, `T?`, and the checker makes you handle `none` before you use it. `??` gives a default, and `if let` and `while let` take the value out.
- **Errors are values.** A function that can fail says so with `!`, and its caller handles the error with `try`, `match` or `!`. Nothing is thrown past code that didn't expect it. `fn main() ! Error` with `try` and `.context(...)` is the shape of most scripts.
- **Integers have sizes.** `i32`, `i64`, `u8` and the rest. `+`, `-` and `*` wrap past the end, as in Java, so code ported from Java keeps working. For integers of any size there's [`std.bigint`](../std/bigint.md).
- **Strings are UTF-8 bytes.** `len` and slices count bytes, as Rust's do, so `s[0..2]` is the first two bytes, and `find` gives the byte offset to slice at. `for r in s.runes()` goes over the characters.
- **A name means one thing in a function.** There's no shadowing, so `let x` can't hide another `x`, and a `let` can't be changed. Use `var` for a value that changes.
- **Copies are written out.** Passing a value lends it, and `.copy()` makes a copy where one is needed, so a script never copies a large list without saying so ([ownership](../reference/ownership.md)).
- **It's fast.** A script compiles to native code through C rather than being interpreted. On our benchmarks, `std.csv` reads CSV more than 6 times as fast as Python's `csv` module, and `std.url` encodes 16 times as fast as `urllib.parse`. Each std page has its numbers.
