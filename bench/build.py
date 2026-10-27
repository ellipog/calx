"""Times how fast Calx builds, the way D64 and the G3.4 gate measure it.

    py -I bench/build.py [--modules 64] [--runs 5] [--record]

It generates a large program, `--modules` modules of about 150 lines each, each importing the
one before it, and times the compiler on it and on a small program and the tick benchmark:

- check: `calx check` on the large program, which runs the whole front end.
- cold: a debug build of the large program into an empty cache, so every module, the runtime
  and the link happen, and the same in release mode.
- one module: a debug build of the large program after changing one module.
- small cold, small unchanged: `calx run` on a small program with an empty cache, then again
  with nothing changed (G3.4: under 0.5 s and 50 ms).
- bench one module: a debug build of the tick benchmark after changing its own module, with the
  standard library's cached (G3.4: under 1 s).

The generated program lives in the temp folder. Each time is the median of `--runs`. --record writes bench/results/build-<platform>.json, with
the machine and the commit (D34). On Linux, use python3 for py.
"""

import argparse
import importlib.util
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.dont_write_bytecode = True  # no __pycache__ in bench/ from loading run.py
spec = importlib.util.spec_from_file_location("bench_run", HERE / "run.py")
bench_run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench_run)
PLATFORM = bench_run.PLATFORM
EXE = bench_run.EXE
CALX = ROOT / "build" / "compiler" / PLATFORM / f"calx{EXE}"
# In the temp folder, so under WSL the program is on Linux's own file system: reading the Windows
# one through /mnt/c is about ten times slower, and would be most of what's measured.
WORK = Path(tempfile.gettempdir()) / f"calx-buildtime-{PLATFORM}"

GOALS = {"small_cold_s": 0.5, "small_unchanged_s": 0.05, "bench_one_module_s": 1.0}


def module(i, tweak=0):
    """Module m<i>: a struct with methods, an enum and a match, lists, strings and loops."""
    imp = f"import m{i - 1}.{{score{i - 1}}}\n\n" if i else ""
    chain = f"    total += score{i - 1}(n / 2)\n" if i else ""
    helpers = []
    calls = []
    for j in range(8):
        helpers.append(f"""fn mix{j}(x: i64, k: i64) -> i64 {{
    var h = x
    for r in 0i64..k {{
        h = (h ^ (r + {j + tweak})) * 1099511628211
        if h % 3 == 0 {{
            h += r
        }}
    }}
    return h
}}
""")
        calls.append(f"    total = mix{j}(total, i64(n))\n")
    return f"""{imp}pub struct Thing{i} {{
    pub id: i32,
    pub weight: f64,
    pub tags: List<String>,
}}

enum Kind{i} {{
    Small
    Large(i32)
    Named(String)
}}

impl Thing{i} {{
    pub fn new(id: i32) -> Thing{i} {{
        var tags = List<String>()
        tags.push("t{{id}}")
        return Thing{i} {{ id: id, weight: f64(id) * 1.5, tags: tags }}
    }}

    pub fn heavier(self, other: Thing{i}) -> bool {{
        return self.weight > other.weight
    }}
}}

fn kind_of(n: i32) -> Kind{i} {{
    if n < 10 {{
        return Kind{i}.Small
    }}
    if n < 100 {{
        return Kind{i}.Large(n)
    }}
    return Kind{i}.Named("n{{n}}")
}}

fn rank(k: Kind{i}) -> i32 {{
    return match k {{
        Small => 1,
        Large(n) => n % 7,
        Named(s) => i32.wrap(s.len()),
    }}
}}

{chr(10).join(helpers)}
pub fn score{i}(n: i32) -> i64 {{
    var total: i64 = 0
    var things = List<Thing{i}>()
    for j in 0..n {{
        things.push(Thing{i}.new(j))
    }}
    for j in 0usize..things.len() {{
        total += i64(rank(kind_of(things[j].id))) + i64.saturate(things[j].weight)
        if j > 0 and things[j].heavier(things[j - 1]) {{
            total += i64.wrap(things[j].tags.len())
        }}
    }}
{chain}{"".join(calls)}    return total
}}
"""


def generate(n, folder):
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    for i in range(n):
        (folder / f"m{i}.calx").write_text(module(i), encoding="utf-8", newline="\n")
    main = f"import m{n - 1}.{{score{n - 1}}}\n\nfn main() {{\n    println(\"{{score{n - 1}(40)}}\")\n}}\n"
    (folder / "main.calx").write_text(main, encoding="utf-8", newline="\n")
    return folder / "main.calx"


def timed(args, cache, cwd=ROOT):
    env = dict(os.environ, CALX_CACHE=str(cache))
    start = time.perf_counter()
    p = subprocess.run([str(CALX)] + args, cwd=cwd, env=env, capture_output=True, text=True)
    took = time.perf_counter() - start
    if p.returncode != 0:
        sys.exit(f"build: {' '.join(args)} failed:\n{p.stdout}{p.stderr}")
    return took


def median_of(runs, measure):
    return statistics.median(measure() for _ in range(runs))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--modules", type=int, default=64)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--record", action="store_true")
    args = parser.parse_args()
    if not CALX.exists():
        sys.exit(f"build: build the compiler first, into {CALX.parent} (see compiler/README.md)")

    WORK.mkdir(parents=True, exist_ok=True)
    big = generate(args.modules, WORK / "big")
    lines = sum(len(p.read_text(encoding="utf-8").splitlines()) for p in big.parent.glob("*.calx"))
    small = WORK / "small.calx"
    small.write_text('fn main() {\n    println("hello")\n}\n', encoding="utf-8", newline="\n")
    out = WORK / f"out{EXE}"
    results = {}

    with tempfile.TemporaryDirectory(prefix="calx-build-") as tmp:
        tmp = Path(tmp)
        warm = tmp / "warm"
        results["check_s"] = median_of(args.runs, lambda: timed(["check", str(big)], warm))

        def cold(mode):
            def run():
                cache = tmp / f"cold{time.perf_counter_ns()}"
                took = timed(["build"] + mode + [str(big), "-o", str(out)], cache)
                shutil.rmtree(cache, ignore_errors=True)
                return took
            return run

        results["cold_debug_s"] = median_of(args.runs, cold([]))
        results["cold_release_s"] = median_of(args.runs, cold(["--release"]))

        # One module changed: m0, which everything imports through the chain, so the change
        # must not ripple into the others' C.
        timed(["build", str(big), "-o", str(out)], warm)
        state = {"tweak": 0}

        def one_module():
            state["tweak"] += 1
            (big.parent / "m0.calx").write_text(module(0, state["tweak"]), encoding="utf-8", newline="\n")
            return timed(["build", str(big), "-o", str(out)], warm)

        results["one_module_debug_s"] = median_of(args.runs, one_module)

        def small_cold():
            cache = tmp / f"small{time.perf_counter_ns()}"
            took = timed(["run", str(small)], cache)
            shutil.rmtree(cache, ignore_errors=True)
            return took

        results["small_cold_s"] = median_of(args.runs, small_cold)
        timed(["run", str(small)], warm)
        results["small_unchanged_s"] = median_of(args.runs, lambda: timed(["run", str(small)], warm))

        # The tick benchmark with its own module changed: a copy whose warmup default differs.
        bench_src = (HERE / "calx" / "tickbench.calx").read_text(encoding="utf-8")
        bench_copy = WORK / "tickbench.calx"
        bench_copy.write_text(bench_src, encoding="utf-8", newline="\n")
        timed(["build", str(bench_copy), "-o", str(out)], warm)

        def bench_one():
            state["tweak"] += 1
            changed = bench_src.replace("warmup: 200,", f"warmup: {200 + state['tweak']},", 1)
            bench_copy.write_text(changed, encoding="utf-8", newline="\n")
            return timed(["build", str(bench_copy), "-o", str(out)], warm)

        results["bench_one_module_s"] = median_of(args.runs, bench_one)

    print(f"calx builds on {PLATFORM}, {args.modules} generated modules ({lines} lines), median of {args.runs}:")
    print("| Measure | Time | G3.4 |")
    print("| --- | --- | --- |")
    for k, v in results.items():
        goal = GOALS.get(k)
        mark = "" if goal is None else (f"under {goal * 1000:.0f} ms: " + ("yes" if v < goal else "NO"))
        print(f"| {k[:-2].replace('_', ' ')} | {v * 1000:.0f} ms | {mark} |")

    if args.record:
        path = HERE / "results" / f"build-{PLATFORM}.json"
        doc = {
            "recorded": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "commit": bench_run.commit(),
            "machine": bench_run.machine(),
            "calx": subprocess.run([str(CALX), "version"], capture_output=True, text=True).stdout.strip(),
            "modules": args.modules,
            "lines": lines,
            "runs": args.runs,
            "seconds": {k[:-2]: round(v, 4) for k, v in results.items()},
            "g3_4": {k[:-2]: v < GOALS[k] for k, v in results.items() if k in GOALS},
        }
        path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"build: wrote {path}")


if __name__ == "__main__":
    main()
