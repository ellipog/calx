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
| `os.geteuid() == 0`, `ctypes.windll.shell32.IsUserAnAdmin()` | `is_elevated()` from [`std.env`](../std/env.md#the-user), on every system |
| `getpass.getuser()`, `os.geteuid()` | `user_name()` and `user_id()` from [`std.env`](../std/env.md#the-user), each optional |
| `input()`, `for line in sys.stdin:` | `Lines()` from [`std.io`](../std/io.md), whose `next()` gives each line, then `none` |
| `print(s)`, `print(s, end="")`, `print(s, file=sys.stderr)` | `println(s)`, `print(s)`, `eprintln(s)` ([printing](../reference/printing.md)) |
| `black`, `ruff format` | `calx fmt`, one format with no settings ([formatting](../reference/command.md#formatting)) |
| `unittest`, `pytest` | `@test` on a function, and `calx test tool.calx`, which runs each test in a process of its own ([testing](../reference/command.md#testing)) |
| `assert x == 1, "why"` | `assert(x == 1, "why")` |
| `pip install requests` | Nothing: `std.http` comes with Calx |
| `python -m http.server` | `calx serve`, on loopback unless `--bind` says otherwise ([serving a folder](../reference/command.md#serving-a-folder)) |

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
| `f"{name} has {n}"`, `f"{x:.2f}"` | `"{name} has {n}"`, `"{x:.2}"`: every string literal interpolates ([printing](../reference/printing.md)). Write `\{` for a brace, as in `s.contains("\{")` for Python's `"{" in s`, or use a raw string `#"..."#` |
| `len(s)` | `s.len()` in bytes of UTF-8, or count the runes with `for r in s.runes()` |
| `s.split(",")`, `s.split()` | `s.split(",")`, `s.split_whitespace()` |
| `s.strip()`, `s.removeprefix("x")` | `s.trim()`, `s.trim_prefix("x")` |
| `s.splitlines()` | `s.lines()` |
| `s.partition("=")` | `s.split_once("=")`, a `(String, String)?` |
| `", ".join(xs)` | `xs.join(", ")` |
| `x in s`, `s.find(t)` | `s.contains(t)`, `s.find(t)`, a `usize?` rather than `-1` |
| `s.lower()` | `s.to_lower_root()` |
| `int(s)`, `float(s)` | `i32.parse(s)`, `f64.parse(s)`, which give `none` for text that isn't one |
| `float(n)`, `int(x)`, `x & 0xff` for a number | `f64(n)` where it's exact, `f64.nearest(n)` where it rounds, `i64.saturate(x)` for a float, `u8.wrap(x)` for the low bits: no number changes type unless the conversion is written ([conversions](../reference/conversions.md)). The checker names the right one for Rust's `x.to_f64()` and the like |
| `re.sub(p, r, s)` | `Regex.python(p)` from [`std.regex`](../std/regex.md), then `replace_all(s, r)`: Python's `re` as it is, its syntax, templates and matches. Or `Regex.new(p)`, whose syntax is Rust's and which searches in linear time, for patterns from outside |
| `re.sub(p, lambda m: ..., s)` | `replace_all_with(s, \|c\| ...)` on a `Regex` from [`std.regex`](../std/regex.md), where `c` is the match's `Captures` |
| `re.match(p, s)`, `re.fullmatch(p, s)` | `match_start(s)` and `full_match(s)` on a `Regex` from [`std.regex`](../std/regex.md), or `captures_start` and `full_captures` for the groups |
| `s.encode()`, `b.decode()` | `s.to_bytes()`, `String.from_utf8(b)`, which gives `none` for bad UTF-8, or [`decode_lossy_unicode`](../std/text-utf8.md) for Python's `errors="replace"` |

## Lists and dicts

| Python | Calx |
| --- | --- |
| `xs = [1, 2]`, `xs.append(3)` | `var xs = [1, 2]`, `xs.push(3)`. A `let` can't change, and a `var` can |
| `xs = []`, room for n | `var xs: List<u8> = []`, with the element type on the variable, and `xs.reserve(n)` |
| `xs[-1]` | `xs[xs.len() - 1]` |
| `xs[1:3]` | `xs[1..3]` |
| `[f(x) for x in xs if p(x)]` | `xs.filter(\|x\| p(x)).map(\|x\| f(x))` ([closures](../reference/closures.md)) |
| `sum(xs)`, `min(xs)`, `max(xs)` | `xs.sum()`, `xs.min()`, `xs.max()`, the last two `none` for an empty list ([lists](../reference/lists.md#sum-min-and-max)) |
| `xs.sort()`, `xs.sort(key=f)` | `xs.sort()`, `xs.sort_by_key(\|x\| f(x))` for a key that's `Copy`, such as a number, or `xs.sort_by(\|a, b\| ...)`. All sort in place, and the last two are stable |
| `any(...)`, `all(...)` | `xs.any(\|x\| ...)`, `xs.all(\|x\| ...)` |
| `for i in range(len(xs)):`, `for i in range(n):` | `for i in 0..xs.len()`, whose `i` is a `usize`, an index. `for i in 0..n` counts in `n`'s type, and `0..10` in `i32`, a bare literal's, so write `0usize..10` to index with it |
| `enumerate(xs)` | `for (i, x) in xs.enumerate()` |
| `zip(xs, ys)` | `for (x, y) in xs.zip(ys)` |
| `dict`, which keeps the order keys went in | `LinkedMap<K, V>` ([linked maps](../reference/linked-maps.md)). A `Map` is faster where the order doesn't matter |
| `d.get(k, 0)` | `d.get(k) ?? 0` |
| `v = d.get(k)`, then `if v is not None:` | `if let v = d.find(k) { ... }`, which works for values that own memory too, and `mut` changes them ([finding a value](../reference/maps.md#finding-a-value)) |
| `set()` | `Set<T>`, or `LinkedSet<T>` to keep the order |
| `collections.deque` | `Deque<T>` ([deques](../reference/deques.md)) |
| `heapq` | `PriorityQueue<T>` ([priority queues](../reference/priority-queues.md)) |
| `None` | `none`, in a type that says it may be missing: `String?` ([optionals](../reference/optionals.md)) |
| `Pt = tuple[float, float]`, `type Pt = tuple[float, float]` | `type Pt = (f64, f64)`, another name for the same type ([type aliases](../reference/types.md#type-aliases)) |

## Files and programs

| Python | Calx, from [`std.fs`](../std/fs.md), [`std.path`](../std/path.md) and [`std.process`](../std/process.md) |
| --- | --- |
| `open(p).read()` | `read_text(p)`, or `read(p)` for bytes |
| `open(p, "w").write(t)` | `write_text(p, t)`, or `write(p, bytes)` |
| `for line in open(p):` | `for line in (try read_text(p) ?? String()).lines()` |
| `f = open(p, "wb")`, `f.write(b)`; `open(p, "rb").read(n)` | `var f = create(p)!`, `f.write(b)!`; `open_read(p)!.read(mut buf, n)!`, a [`std.io`](../std/io.md#reader-and-writer) `Writer` and `Reader`, so `std.tar` and `std.zstd` stream into and out of them |
| `os.open(p, os.O_WRONLY \| os.O_CREAT \| os.O_EXCL \| os.O_NOFOLLOW, 0o600)`, `open(p, "x")` | `create_new(p, 0o600)!`, a `File`: `Exists` if anything's at `p`, a link too, and the mode exact whatever the umask ([Saving safely](../std/fs.md#saving-safely)) |
| A function taking any file object, `shutil.copyfileobj(src, dst)` | `fn copy<R: Reader, W: Writer>(src: mut R, dst: mut W)` over [`std.io`](../std/io.md#reader-and-writer)'s interfaces, which files, `List<u8>`, `ByteReader` (`io.BytesIO`), standard input and output, connections and the zstd and tar streams have |
| `os.listdir(d)`, `glob.glob("**/*.py", recursive=True)` | `list(d)`, `glob("**/*.py")`, sorted |
| `os.walk(d)` | `walk(d)` |
| `os.makedirs(d, exist_ok=True)` | `make_dir_all(d)` |
| `shutil.copy`, `shutil.rmtree` | `copy`, `remove_dir_all` |
| `os.path.join`, `pathlib.Path(p).suffix` | `path.join`, `path.file_extension` |
| `tempfile.mkdtemp()` | `temp_dir()` |
| `shutil.disk_usage("/")` | `unsafe { disk_usage("/") }`, with `total`, `used` and `available` |
| `subprocess.run(["git", "status"], check=True)` | `Command.new("git").arg("status").run()` |
| `subprocess.check_output(...)` | `Command.new(...).read()`, its output as text |
| `subprocess.run(..., capture_output=True)` | `Command.new(...).output()`, with the status and both streams |
| `Popen(cmd, start_new_session=True)`, `os.killpg(p.pid, signal.SIGTERM)`, `CREATE_NEW_PROCESS_GROUP` with `p.send_signal(signal.CTRL_BREAK_EVENT)` | `Command.new(...).own_group().spawn()` and `child.stop()`, the same call on every system, then `child.kill_tree()` for what's left, which Python needs `psutil` for ([stopping it](../std/process.md#stopping-it-and-what-it-started)) |
| `Popen(cmd, preexec_fn=lambda: libc.prctl(PR_SET_PDEATHSIG, SIGKILL))`, Linux only | `Command.new(...).ends_with_this().spawn()`, on every system, with its whole group if it's `own_group` too ([ending with this program](../std/process.md#ending-with-this-program)) |
| No standard library way; `ctypes` against `CreateAppContainerProfile` on Windows, `sandbox-exec` on macOS, Landlock on Linux | `Command.new(...).confine(Confine.new("a").write(dir)).spawn()`, which sees only the folders it's given, on every system ([confining it](../std/process.md#confining-it)) |
| `p.wait(timeout=10)`, `p.send_signal(signal.SIGHUP)` | `child.wait_timeout(10000)`, `none` if it's still running, and `child.signal(1)` |
| `shlex.split(line)`, `shlex.quote(w)`, `shlex.join(words)` | `split(line)`, `quote(w)` and `join(words)` from [`std.shell`](../std/shell.md) |
| `os.wait4(pid, 0)`, `psutil.Process(pid).memory_info()` | `unsafe { status.usage() }`, `unsafe { child.usage() }`: CPU time and memory, the same on both systems, without `ctypes` |
| `os.sched_setaffinity(0, cpus)`, `psutil.cpu_times()` | `set_affinity(cpus)`, or `c.affinity(cpus)` for a program it starts, and `unsafe { machine_cpu() }`, on Windows too, from [`std.process`](../std/process.md#pinning-and-the-machines-load) |
| `logging.basicConfig(level=logging.INFO, filename=p)`, `logging.info(m)`, `logging.getLogger(n)` | `config(level: Level.Info, file: p)`, `info(m)` and `logger(n)` from [`std.log`](../std/log.md), with Python's formats |
| `signal.signal(signal.SIGTERM, on_stop)`, `signal.SIGINT` | `handle()` from [`std.signal`](../std/signal.md), then `take()` or `wait()` where the program is ready to stop, rather than a handler that runs anywhere |
| `loop.add_signal_handler(SIGTERM, stop)`, `server.shutdown()` | `net.wake_on_signal()`, or `server.wake_on_signal()` and `server.shutdown()` from [`std.http.server`](../std/http-server.md#stopping-cleanly), whose `next` gives `none` at a signal |
| `lib = ctypes.CDLL(path)`, `lib.name`, `ctypes.CFUNCTYPE` | `unsafe { CLibrary.open(path) }` from [`std.c`](../std/c.md), `unsafe { lib.symbol(name) }`, and `to_fn<extern "C" fn(...) -> T>()` on the address it gives |
| pywin32's `win32serviceutil.ServiceFramework` | `service()` from [`std.signal`](../std/signal.md#windows-services), which runs the program as a Windows service, its stop coming as `Terminate` |

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
| `tomllib.loads(t)`, `tomli_w.dumps(d)` | `parse(t)` and `write(d)` from [`std.toml`](../std/toml.md), with `doc.path("tool.black.line-length")` for a dotted key. `@toml` on a struct reads a config file straight into it |
| `yaml.safe_load(t)`, `yaml.safe_load_all(t)`, `yaml.safe_dump(v, sort_keys=False)` | `parse(t)`, `parse_all(t)` and `write(v)` from [`std.yaml`](../std/yaml.md), PyYAML's YAML 1.1 with its quirks, written byte for byte as PyYAML writes it, with `doc.path("servers.0.port")` for a value deep in |
| A dataclass read from YAML, `Config(**yaml.safe_load(t))` | `@yaml struct Config { ... }`, then `Config.from_yaml_text(t)`, which checks each field's type and names the path to what's wrong ([`@yaml`](../std/yaml.md#yaml)) |
| `ConfigParser().read_string(t)`, `config.get(s, k)`, `getint`, `config.write(f)` | `parse(t)`, `ini.get(s, k)`, `get_int` and `ini.write()` from [`std.ini`](../std/ini.md) |
| `csv.reader(f)`, `csv.DictReader(f)`, `csv.writer(f)` | `parse`, `parse_records` and `format` from [`std.csv`](../std/csv.md), or `Reader` for a row at a time |
| `xml.etree.ElementTree.fromstring(t)`, `e.findall(p)`, `e.iterfind(p)`, `ET.tostring(e)` | `parse(t)`, `e.find_all(p)`, `e.find_each(p, \|x\| ...)` and `format(e)` from [`std.xml`](../std/xml.md), with ElementTree's paths |
| `html.escape(t)`, `html.unescape(t)` | `escape(t)` and `unescape(t)` from [`std.html`](../std/html.md) |
| An `html.parser.HTMLParser` subclass with `handle_starttag` and the rest | `for t in tokens(page)` and a `match` on `Start`, `End`, `Text` and the rest, or `start_tags(page)`, from [`std.html`](../std/html.md#reading-html) |
| `textwrap.fill(t, 70)`, `textwrap.dedent(t)`, `TextWrapper(width=40, ...)` | `fill(t, 70)`, `dedent(t)` and `Wrapper { width: 40, ... }` from [`std.textwrap`](../std/textwrap.md) |
| `unicodedata.normalize("NFC", t)`, `unicodedata.is_normalized` | `normalize(t, Form.NFC)` and `is_normalized` from [`std.unicode`](../std/unicode.md) |
| `unicodedata.name(c, None)`, `unicodedata.lookup(n)`, `"\N{SNOWMAN}"` | `name(r)` and `lookup(n)` from [`std.unicode.names`](../std/unicode-names.md), which give `none` where Python raises. Calx's strings have no `\N{...}`; write the rune or `\u{2603}` |
| `sqlite3.connect(p)`, `execute`, `executemany`, `fetchall` | `Database.open(p)`, then `execute`, `execute_many` and `query` from [`std.sqlite`](../std/sqlite.md), with SQLite built in and each statement its own transaction unless it's between `begin` and `commit` |
| `urllib.parse.quote`, `urlsplit`, `urljoin` | `encode`, `Url.parse`, `url.join` from [`std.url`](../std/url.md), or its `urlsplit`, `urljoin` and `urldefrag`, which give Python's results to the letter |
| `jsonschema.validate(i, s)`, `Draft7Validator(s).iter_errors(i)`, `best_match(errors)` | `validate(i, s)`, `Validator.new(s, Draft.Draft7)` then `v.iter_errors(i)`, and `best_match(errors)` from [`std.json.schema`](../std/json-schema.md), with the same errors, messages, paths and order |
| `uuid.uuid4()` | `Uuid.random()` from [`std.uuid`](../std/uuid.md) |
| `requests.get(url).text` | `fetch(url)` from [`std.http`](../std/http.md), then `String.from_utf8(resp.body)`, ready for `parse` |
| `requests.post(url, data={...}, files={...})` | `post_form(url, headers, Form.new().field(n, v).file(n, filename, bytes), timeout_ms, limit)` from [`std.http`](../std/http.md), a `multipart/form-data` body with its boundary and quoting done |
| `httpx.Client(http2=True)`, `c.get(url)`, `r.http_version` | `get(url, headers, timeout_ms, limit, version: Version.Http2)` from [`std.http`](../std/http.md#http2), then `resp.version`. The whole program is one client, so connections are kept, and HTTP/2 requests to a host go over one at once. `http1=False` is `Version.Http2Only` |
| `requests.Session()`, `r.elapsed` | Nothing for the session: every request keeps its connection for the next ([connections](../std/http.md#connections)). `resp.timings` has the time to connect, finish TLS and get the first byte, as curl's `-w` gives them ([timings](../std/http.md#timings)) |
| `with httpx.stream("POST", url, json=req) as r:`, `r.iter_lines()`, `r.iter_bytes()` | `stream_post(url, headers, body)` or `stream_get(url)` from [`std.http`](../std/http.md#reading-a-response-as-it-comes), then `s.head()`, and `s.read_line()` or `s.read()` until `none`. `s.close()`, or dropping it, ends it as leaving the `with` does |
| `httpx_sse.connect_sse(c, "GET", url)`, `EventSource(r).iter_sse()`, `ev.event`, `ev.data`, `ev.id` | `connect(url)` or `connect_post(url, headers, body)` from [`std.http.sse`](../std/http-sse.md), then `es.next()` until `none`, with `ev.event`, `ev.data`, `ev.id` and `ev.retry`, and `es.reconnect()`, which sends `Last-Event-ID` |
| `http.server.HTTPServer`, `BaseHTTPRequestHandler`, `do_GET` | `Server.listen(port)`, then `next()` for each request and `respond(req, res)` from [`std.http.server`](../std/http-server.md) |
| `websockets.connect(url)`, `await ws.send(m)`, `await ws.recv()` | `connect(url)`, `ws.send_text(m)`, `ws.receive(timeout_ms)` from [`std.http.websocket`](../std/http-websocket.md), on a thread of its own rather than `asyncio` |
| `websockets.connect(url, ping_interval=20, ping_timeout=25)` | `connect(url, ping_interval_ms: 20000, ping_timeout_ms: 25000)`: a connection that has gone quiet is pinged, and one that stays quiet ends with `Timeout` ([a dead connection](../std/http-websocket.md#a-dead-connection)) |
| `websockets.serve(handler, host, port)`, `await ws.send(m)`, `async for m in ws` | `server.upgrade(req)`, `server.send_text(s, m)`, and each message from `server.next_event()` in [`std.http.server`](../std/http-server.md#websockets) |
| A `text/event-stream` response kept open, written to as events happen | `server.stream(req)`, then `server.send_event(s, data)` ([event streams](../std/http-server.md#event-streams)) |
| `socket(AF_INET, SOCK_DGRAM)`, `s.bind((host, port))`, `s.sendto(b, addr)`, `s.recvfrom(65535)` | `net.bind_udp(host, port)`, `u.send_to(b, addr)`, and `u.receive_from()` for each `Datagram` event, from [`std.net`](../std/net.md#udp), in the same loop as its TCP connections, as `selectors` would have them |
| `setsockopt(IPPROTO_IP, IP_ADD_MEMBERSHIP, mreq)`, `IP_MULTICAST_TTL`, `IP_MULTICAST_LOOP`, `SO_REUSEADDR` | `u.join_multicast(group)`, `set_multicast_ttl(n)`, `set_multicast_loopback(on)`, and `bind_udp(host, port, reuse: true)` ([multicast](../std/net.md#multicast)), the same on IPv4 and IPv6, with no `struct.pack` |
| `ipaddress.ip_address(t)`, `.is_loopback`, `psutil.net_if_addrs()`, `socket.gethostname()` | `Ip.parse(t)`, `ip.is_loopback()`, `interfaces()` and `host_name()` from [`std.net`](../std/net.md#interfaces) |
| zeroconf's `Zeroconf()`, `register_service(ServiceInfo(...))`, `ServiceBrowser(zc, type_, listener)`, `get_service_info`, `close()` | `Mdns.new()`, `m.register(ServiceInfo.new(kind, name, port))`, `m.browse(kind)` and each `MdnsEvent` from `m.next_event()`, `m.resolve(kind, name)` and `m.close()` from [`std.net.mdns`](../std/net-mdns.md), or `find(kind)` to look once. A taken name becomes `Name (2)` unless it's told not to, as Bonjour names it |
| `hashlib.sha256(b).hexdigest()` | `hex(sha256(b))`, with [`std.hash`](../std/hash.md) and [`std.bytes`](../std/bytes.md) |
| `hashlib.blake2b(b, digest_size=32, key=k).digest()`, `h.update(b)` | `blake2b(b, 32, key: k)`, or `Blake2b.new(32, key: k)` and `h.add(b)`, from [`std.hash`](../std/hash.md#blake2b) |
| `hashlib.blake2s(b, digest_size=16, key=k).digest()` | `blake2s(b, 16, key: k)`, or `Blake2s.new(16, key: k)`, from [`std.hash`](../std/hash.md#blake2s) |
| argon2-cffi's `ph = PasswordHasher()`, `ph.hash(pw)`, `ph.verify(h, pw)`, `ph.check_needs_rehash(h)` | `let ph = PasswordHasher()`, `ph.hash(pw)`, `verify_password(h, pw)` and `ph.needs_rehash(h)` from [`std.crypto`](../std/crypto.md#passwords), the same PHC strings, with `false` where `verify` raises |
| `hmac.compare_digest(a, b)` | `compare_digest(a, b)` from [`std.crypto`](../std/crypto.md#passwords) |
| cryptography's `Ed25519PrivateKey.from_private_bytes(seed)`, `k.sign(m)`, `Ed25519PublicKey.from_public_bytes(p).verify(sig, m)` | `Ed25519Key.from_seed(seed)!`, `k.sign(m)` and `Ed25519PublicKey.from_bytes(p)!.verify(m, sig)` from [`std.crypto`](../std/crypto.md#ed25519), with `false` where `verify` raises `InvalidSignature` |
| cryptography's `load_pem_private_key(p8, None)`, `k.sign(m, ec.ECDSA(hashes.SHA256()))`, `decode_dss_signature(der)`, `pub.verify(der, m, ec.ECDSA(hashes.SHA256()))` | `P256Key.from_pem(p8)!`, then `k.sign(m)` for JWS's 64-byte r and s or `k.sign_der(m)` for DER, and `p.verify(m, sig)` or `p.verify_der(m, der)` from [`std.crypto`](../std/crypto.md#es256), with RFC 6979's nonces, so the same message signs the same, and `false` where `verify` raises `InvalidSignature` |
| cryptography's `X25519PrivateKey.generate()`, `k.exchange(X25519PublicKey.from_public_bytes(p))`, `k.public_key().public_bytes_raw()` | `X25519Key.generate()`, `k.exchange(X25519PublicKey.from_bytes(p)!)` and `k.public_key().bytes()` from [`std.crypto`](../std/crypto.md#x25519), with `none` where `exchange` raises `ValueError` for a key of small order |
| noiseprotocol's `NoiseConnection.from_name(b'Noise_IK_25519_ChaChaPoly_SHA256')`, `set_as_initiator()`, `set_keypair_from_private_bytes(Keypair.STATIC, k)`, `write_message(p)`, `read_message(m)`, `encrypt(d)`, `decrypt(d)` | `NoiseHandshake.initiator("Noise_IK_25519_ChaChaPoly_SHA256")`, `h.set_static(key)`, `h.write_message(p)`, `h.read_message(m)`, then `let t = h.transport()` and `t.encrypt(d)` and `t.decrypt(d)` from [`std.crypto`](../std/crypto.md#noise), with a `NoiseError` or `none` where it raises |
| cryptography's `ChaCha20Poly1305(key).encrypt(nonce, data, aad)`, `.decrypt(nonce, sealed, aad)`, `ChaCha20Poly1305.generate_key()` | `ChaCha20Poly1305.new(key).encrypt(nonce, data, aad)`, `.decrypt(nonce, sealed, aad)` and `ChaCha20Poly1305.generate_key()` from [`std.crypto`](../std/crypto.md#authenticated-encryption), the same bytes, with `none` where `decrypt` raises `InvalidTag`. `XChaCha20Poly1305` takes a 24-byte nonce |
| cryptography's `AESGCM(key).encrypt(nonce, data, aad)`, `AESGCM.generate_key(bit_length=256)` | `AesGcm.new(key).encrypt(nonce, data, aad)`, `.decrypt(...)` and `AesGcm.generate_key(256)` from [`std.crypto`](../std/crypto.md#authenticated-encryption) |
| `base64.b64encode(b)` | `base64(b)` from [`std.bytes`](../std/bytes.md) |
| `keyring.set_password(s, u, pw)`, `get_password`, `delete_password` | `set(s, u, pw)`, `get(s, u)` and `delete(s, u)` from [`std.keyring`](../std/keyring.md), in Credential Manager, the Keychain or the Secret Service where keyring keeps them, with `none` and `false` for a secret that isn't there and `Unavailable` where there's no store |
| `zipfile`, `tarfile`, `gzip` | [`std.zip`](../std/zip.md), [`std.tar`](../std/tar.md), [`std.zlib`](../std/zlib.md) |
| nbtlib's `nbtlib.load(p)`, `f.save()`, `f["Data"]["DayTime"]`, `str(tag)` | `NbtFile.open(p)`, `f.save(p)`, `f.root.get("Data")!.get("DayTime")` or a `match` in place, and `"{tag}"` from [`std.nbt`](../std/nbt.md), with vanilla's SNBT, and 20 to 66 times as fast on [its benchmark](../std/nbt.md#speed) |
| nbtlib's `nbtlib.parse_nbt(text)`, `serialize_tag(tag, indent=4)` | `parse_snbt(text)!` and `tag.to_pretty_snbt()` from [`std.nbt`](../std/nbt.md#snbt), which read and lay out SNBT as vanilla 26.3 does |
| nbtlib's `tag[Path("a.b[0]")]`, `tag[path] = v`, `del tag[path]` | `NbtPath.parse("a.b[0]")!` and its `get(tag)`, `set(mut tag, v)` and `remove(mut tag)` from [`std.nbt`](../std/nbt.md#nbt-paths), which follow paths as vanilla's `/data` does |
| ftb-snbt-lib's `slib.load(fp)`, `slib.loads(text)`, `slib.dumps(tag)` | `FtbSnbt.open(p)`, `parse_ftb_snbt(text)!` and `f.to_snbt()` from [`std.nbt`](../std/nbt.md#ftbs-snbt), which read and write FTB's quests and configs as FTB Library does, `true` and `false` kept |
| `lzma.decompress(b)` | `decompress(b)` from [`std.xz`](../std/xz.md), with the same errors and a limit if it's given |
| `lzma.compress(b, preset=9)` | `compress(b, 9)` from [`std.xz`](../std/xz.md), on all the processors, the same bytes on any machine |
| `LZMADecompressor()`, `lzma.open(path)` to read | `Decompressor.new()` from [`std.xz`](../std/xz.md#a-piece-at-a-time): `d.add(piece)`, then `d.finish()`, which reads every stream as `lzma.open` does |
| `LZMACompressor()`, `lzma.open(path, "w")` | `Compressor.new()` from [`std.xz`](../std/xz.md#a-piece-at-a-time): `c.add(piece)`, then `c.finish()`, the same bytes as `compress` |
| `bz2.decompress(b)`, `tarfile.open(p)` on a `.tar.bz2` | `decompress(b)` from [`std.bz2`](../std/bz2.md), with the same errors and a limit if it's given, every stream in it; `Tar.open(fs.read(p)!)` from [`std.tar`](../std/tar.md) |
| `bz2.compress(b, 9)` | `compress(b, 9)` from [`std.bz2`](../std/bz2.md#compressing), the same bytes, several blocks at once |
| `compression.zstd.decompress(b)`, `ZstdDict(d)` | `decompress(b)` and `Dictionary.new(d)` from [`std.zstd`](../std/zstd.md), with the same errors, big frames on all the processors |
| `compression.zstd.compress(b, 19)`, `compress(b, 19, zstd_dict=d)` | `compress(b, 19)` and `d.compress(b, 19)` from [`std.zstd`](../std/zstd.md#compressing), big inputs on all the processors, the same bytes on any machine |
| `ZstdCompressor()`, `c.compress(piece)`, `c.flush()`; `ZstdDecompressor()`, `d.decompress(piece)` | `Compressor.new()`, `c.add(piece)`, `c.finish()`, and `c.flush()` for `FLUSH_BLOCK`; `Decompressor.new()`, `d.add(piece)!`, `d.finish()!`, from [`std.zstd`](../std/zstd.md#a-piece-at-a-time), every frame to the end |
| zstandard's `ZstdCompressor().stream_writer(f)`, `ZstdDecompressor().stream_reader(f)` | `zstd.Writer.new(f)` and `zstd.Reader.new(f)` from [`std.zstd`](../std/zstd.md#a-piece-at-a-time), over any `std.io` `Writer` or `Reader`, in as much memory as the level's window takes |
| `tarfile.open(fileobj=f, mode="w\|")`, `t.add(path, arcname=n)`, `t.addfile(info, fileobj)` | `StreamWriter.new(f)`, `t.add_tree(n, path)!` or `t.add_file(n, path)!`, `t.add_from(n, size, mut r)!`, then `t.finish()!`, from [`std.tar`](../std/tar.md#a-piece-at-a-time), each entry written as it's added |
| `tarfile.open(fileobj=f, mode="r\|")`, `for m in t:`, `t.extractfile(m).read()`, `t.extractall(d, filter="data")` | `StreamReader.new(f)`, `while let e = r.next()! {`, `r.read_all()!` or `r.read(mut buf, n)!`, `r.extract(d)!`, from [`std.tar`](../std/tar.md#a-piece-at-a-time), refusing paths out of `d` |
| `sock.makefile("rwb")` | `Stream.new(net, conn)` from [`std.net`](../std/net.md#a-connection-as-a-stream), which waits as a blocking socket does |
| `train_dict(samples, 16384)`, `finalize_dict(d, samples, 16384, 3)`, `d.dict_content` | `train_dict(samples, 16384)`, `finalize_dict(d, samples, 16384, 3)` and `d.to_bytes()` from [`std.zstd`](../std/zstd.md#training-one), 3 to 5 times as fast |
| Pillow's `Image.open(p)`, `im.resize((w, h))`, `im.save(p)` | `Image.open(p)`, `im.resize(w, h)` and `im.save(p)` from [`std.image`](../std/image.md), with Pillow's pixels |
| `draw = ImageDraw.Draw(im)`, `draw.rectangle([x0, y0, x1, y1], fill=a, outline=b, width=2)`, `draw.line(pts, fill=c, width=3)`, `draw.point((x, y), fill=c)` | `im.rectangle(x0, y0, x1, y1, Paint.both(a, b, 2))`, `im.line(pts, c, 3)` and `im.point(x, y, c)` from [`std.image`](../std/image.md#shapes), with Pillow's pixels; `ellipse`, `polygon`, `rounded_rectangle`, `arc` and `pieslice` too |
| `ImageFont.truetype(p, 24)`, `ImageDraw.Draw(im).text((x, y), t, font=f, fill=c)` | `Font.open(p)` from [`std.font`](../std/font.md), then `im.draw_text(x, y, t, font, 24.0, c)`; `font.measure(t, 24.0)` for `f.getlength(t)` |
| `wave.open(p)`, `w.getnchannels()`, `w.readframes(w.getnframes())`, then `struct.unpack(f"<{n}h", raw)` | `Wave.open(p)` from [`std.wave`](../std/wave.md), then `w.channels`, `w.frames`, and `w.samples()` for the numbers, or `w.samples_f32()` from -1 to 1 |
| `wave.open(p, "wb")`, `setnchannels`, `setsampwidth`, `setframerate`, `writeframes` | `Wave.from_i16(channels, rate, samples).save(p)` or `Wave.new(channels, rate, width, frames).save(p)` from [`std.wave`](../std/wave.md), the same bytes |
| `datetime.now()`, `strftime`, `strptime` | `unsafe { Timestamp.now() }`, `format` and `parse` from [`std.time`](../std/time.md), with Python's codes |
| `z = ZoneInfo("Europe/Oslo")`, `datetime.fromtimestamp(s, z)`, `datetime(2026, 3, 29, 8, tzinfo=z).timestamp()` | `TimeZone.named("Europe/Oslo")!`, `z.date_time(t)` and `z.timestamp(d)` from [`std.time`](../std/time.md#time-zones), with the tz database built in, so on Windows too, and Python's moments in gaps and overlaps |
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
