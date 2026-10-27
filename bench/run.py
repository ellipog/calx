"""Builds and runs the tick-loop benchmark the way docs/benchmarks.md measures it.

    py -I bench/run.py rust|calx [--threads 1,24] [--runs 10] [--warmup 200] [--ticks 1000] [--record]

Each run is a fresh process. The result for a configuration is the median, over its runs, of the
mean time per measured tick. Every run must print the checksum in bench/expected.json, or the
result doesn't count. --record writes bench/results/<impl>-<platform>.json, with the machine, the
toolchain and the commit beside the numbers (D34). On Linux, use python3 for py.
"""

import argparse
import json
import os
import platform
import statistics
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PLATFORM = "windows" if os.name == "nt" else sys.platform
BUILD = ROOT / "build" / "bench" / PLATFORM
EXPECTED = HERE / "expected.json"
EXE = ".exe" if os.name == "nt" else ""


def output(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, check=True, **kw).stdout.strip()


def build_rust():
    target = BUILD / "rust"
    env = dict(os.environ, CARGO_TARGET_DIR=str(target))
    subprocess.run(["cargo", "build", "--release", "--quiet"], cwd=HERE / "rust", env=env, check=True)
    version = output(["rustc", "-vV"])
    fields = dict(line.split(": ", 1) for line in version.splitlines()[1:] if ": " in line)
    toolchain = {"rustc": fields.get("release"), "llvm": fields.get("LLVM version")}
    return target / "release" / f"tickbench{EXE}", toolchain


def build_calx():
    calx = ROOT / "build" / "compiler" / PLATFORM / f"calx{EXE}"
    if not calx.exists():
        sys.exit(f"run: build the compiler first, into {calx.parent} (see compiler/README.md)")
    exe = BUILD / "calx" / f"tickbench{EXE}"
    exe.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(calx), "build", "--release", str(HERE / "calx" / "tickbench.calx"), "-o", str(exe)],
                   cwd=ROOT, check=True)
    clang = os.environ.get("CALX_CC") or "clang"    # what the driver compiles with
    try:
        clang_version = output([clang, "--version"]).splitlines()[0]
    except (OSError, subprocess.CalledProcessError):
        clang_version = None
    return exe, {"calx": output([str(calx), "version"]), "commit": commit(), "clang": clang_version}


BUILDERS = {"rust": build_rust, "calx": build_calx}


def physical_cores():
    if os.name == "nt":
        out = output(["powershell", "-NoProfile", "-Command",
                      "(Get-CimInstance Win32_Processor | Measure-Object NumberOfCores -Sum).Sum"])
        return int(out)
    cores = set()
    block = {}
    for line in Path("/proc/cpuinfo").read_text().splitlines() + [""]:
        if not line.strip():
            if "core id" in block:
                cores.add((block.get("physical id"), block["core id"]))
            block = {}
        elif ":" in line:
            k, v = line.split(":", 1)
            block[k.strip()] = v.strip()
    return len(cores) or os.cpu_count()


def machine():
    info = {"platform": PLATFORM, "python": platform.python_version(), "os": platform.platform()}
    if os.name == "nt":
        ps = ("$c = Get-CimInstance Win32_Processor; $m = Get-CimInstance Win32_ComputerSystem; "
              "\"$($c.Name)|$(($c | Measure-Object NumberOfCores -Sum).Sum)|"
              "$(($c | Measure-Object NumberOfLogicalProcessors -Sum).Sum)|$($m.TotalPhysicalMemory)\"")
        name, cores, threads, memory = output(["powershell", "-NoProfile", "-Command", ps]).split("|")
        info.update(cpu=name.strip(), cores=int(cores), threads=int(threads), memory_gib=round(int(memory) / 2**30, 1))
    else:
        cpuinfo = Path("/proc/cpuinfo").read_text()
        name = next((l.split(":", 1)[1].strip() for l in cpuinfo.splitlines() if l.startswith("model name")), "")
        mem = next((l for l in Path("/proc/meminfo").read_text().splitlines() if l.startswith("MemTotal")), "0 0")
        info.update(cpu=name, cores=physical_cores(), threads=os.cpu_count(),
                    memory_gib=round(int(mem.split()[1]) / 2**20, 1), kernel=platform.release())
        if "microsoft" in platform.release().lower():
            info["note"] = "WSL2: the guest's CPU topology isn't the host's, so the thread counts come from Windows"
    return info


def commit():
    try:
        return output(["git", "rev-parse", "HEAD"], cwd=ROOT)
    except (OSError, subprocess.CalledProcessError):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("impl", choices=sorted(BUILDERS))
    parser.add_argument("--threads", help="comma list; default 1 and the number of physical cores")
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=200)
    parser.add_argument("--ticks", type=int, default=1000)
    parser.add_argument("--record", action="store_true", help="write bench/results/<impl>-<platform>.json")
    args = parser.parse_args()

    exe, toolchain = BUILDERS[args.impl]()
    threads = [int(t) for t in args.threads.split(",")] if args.threads else [1, physical_cores()]
    expected = json.loads(EXPECTED.read_text(encoding="utf-8"))["checksum"]
    full = (args.warmup, args.ticks) == (200, 1000)

    configs = []
    for n in threads:
        runs = []
        for i in range(args.runs):
            line = output([str(exe), "--threads", str(n), "--warmup", str(args.warmup), "--ticks", str(args.ticks)])
            run = json.loads(line.splitlines()[-1])
            if full and run["checksum"] != expected:
                sys.exit(f"run: checksum {run['checksum']} isn't the expected {expected}; the result doesn't count")
            runs.append(run)
            print(f"  {args.impl} threads={n} run {i + 1}/{args.runs}: {run['mean_tick_us']:.1f} us/tick", flush=True)
        if len({r["checksum"] for r in runs}) != 1:
            sys.exit("run: runs disagree on the checksum")
        configs.append({
            "threads": n,
            "median_mean_tick_us": round(statistics.median(r["mean_tick_us"] for r in runs), 3),
            "median_p99_tick_us": round(statistics.median(r["p99_tick_us"] for r in runs), 3),
            "runs_mean_tick_us": [r["mean_tick_us"] for r in runs],
            "checksum": runs[0]["checksum"],
        })

    print(f"\n{args.impl} on {PLATFORM}, {args.warmup} warmup + {args.ticks} measured ticks, {args.runs} runs each:")
    print("| Threads | Median of mean tick | Median p99 |")
    print("| --- | --- | --- |")
    for c in configs:
        print(f"| {c['threads']} | {c['median_mean_tick_us'] / 1000:.3f} ms | {c['median_p99_tick_us'] / 1000:.3f} ms |")

    if args.record:
        if not full:
            sys.exit("run: only the full 200 + 1000 tick run is recorded")
        result = {
            "impl": args.impl, "recorded": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "commit": commit(), "machine": machine(), "toolchain": toolchain,
            "warmup": args.warmup, "ticks": args.ticks, "runs": args.runs, "configs": configs,
        }
        out = HERE / "results" / f"{args.impl}-{PLATFORM}.json"
        out.parent.mkdir(exist_ok=True)
        out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"\nrun: wrote {out}")


if __name__ == "__main__":
    main()
