"""The compiler test suite (P3.9): small Calx programs with their expected results.

    py -I tests/run.py                    # build the compiler, then run every test in debug mode
    py -I tests/run.py --modes debug,release
    py -I tests/run.py tests/integers     # only the tests under a folder, or single files
    py -I tests/run.py --no-build         # use the compiler as it is

A test is a `.calx` file under `tests/`, or a fenced `calx` block in `docs/reference/`,
`docs/std/` or `docs/guide/` (D41). It says what should happen in comments:

    println("{1 + 2}")    // out: 3
    let x: u8 = 300       // error: doesn't fit in `u8`
    panic("no")           // panic: no

- `// out: TEXT` is one line of expected output. All of them together, in order, must be exactly
  what the program prints. A program with no `out` lines must print nothing.
- `// error: TEXT` expects an error on that line whose message contains TEXT. A test with any
  `error` lines must fail to compile, with exactly those errors and no others.
- `// panic: TEXT` expects the program to panic on that line, with TEXT in the message.
- `// trace: NAME:LINE` is one frame of the panic's stack trace, innermost first, like `out`:
  all of them together must be the trace's functions and lines, which leave out the file, since
  a module from the standard library is shown by its whole path.
- `// stderr: TEXT` is one line of expected standard error, like `out`. A program that doesn't
  panic must write exactly those lines there, so one with none must write nothing.
- `// stdin: TEXT` is one line of the program's input, each followed by a newline. Without any,
  the input is empty.
- `// args: TEXT` is one command-line argument, passed after `--`, so it can hold spaces.
- `// status: N` is the exit status the program must end with, which is 0 without one.

A test can be several modules. A line `// file: NAME.calx` starts each one, and the first is the
main module. Each file keeps its lines' numbers from the whole test, so errors in any of them are
expected the same way.

Each test that expects errors is also checked with `calx check --json --stdin`, the form editors
use, which must report the same errors at the same places.

A test that runs longer than two minutes (RUN_SECONDS) fails, and its program is stopped.

On Linux, use `python3` for `py`.
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import platform
import re
import signal
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
PLATFORM = "windows" if os.name == "nt" else "linux"
BUILD = ROOT / "build" / "compiler" / PLATFORM
CALX = BUILD / ("calx.exe" if os.name == "nt" else "calx")
DOC_DIRS = [ROOT / "docs" / "reference", ROOT / "docs" / "std", ROOT / "docs" / "guide"]

EXPECT = re.compile(r"//\s*(out|error|panic|trace|stderr|stdin|args|status):(.*)$")
FILE = re.compile(r"^// file: (\S+\.calx)\s*$")
DIAG = re.compile(r"^(.*?):(\d+):(\d+): error: (.*)$")
PANIC = re.compile(r"^panic at (.*?):(\d+):(\d+): (.*)$")
FRAME = re.compile(r"^\tat (.*)\((.*):(\d+)\)$")
RUN_SECONDS = 120


class Test:
    def __init__(self, name, source, path=None):
        self.name = name
        self.source = source
        self.path = path  # a file on disk, or None for a docs example
        self.out = []
        self.errors = []  # (line, text)
        self.panic = None  # (line, text)
        self.trace = []  # "name:line", innermost first
        self.stderr = []
        self.stdin = []
        self.args = []
        self.status = 0
        self.files = []  # (name, text) for a test of several modules, the main one first
        lines = source.splitlines()
        starts = [(i, FILE.match(l).group(1)) for i, l in enumerate(lines) if FILE.match(l)]
        for k, (i, name) in enumerate(starts):
            end = starts[k + 1][0] if k + 1 < len(starts) else len(lines)
            # Blank lines first, so each file's line numbers are the test's.
            self.files.append((name, "\n" * (i + 1) + "\n".join(lines[i + 1:end]) + "\n"))
        for n, line in enumerate(lines, 1):
            m = EXPECT.search(line)
            if not m:
                continue
            kind, text = m.group(1), m.group(2)
            text = text[1:] if text.startswith(" ") else text
            if kind == "out":
                self.out.append(text)
            elif kind == "stderr":
                self.stderr.append(text)
            elif kind == "stdin":
                self.stdin.append(text)
            elif kind == "args":
                self.args.append(text)
            elif kind == "status":
                self.status = int(text)
            elif kind == "trace":
                self.trace.append(text.strip())
            elif kind == "error":
                self.errors.append((n, text.strip()))
            else:
                self.panic = (n, text.strip())


def build_compiler():
    if not (BUILD / "build.ninja").exists():
        cmd = ["cmake", "-S", str(ROOT / "compiler"), "-B", str(BUILD), "-G", "Ninja"]
        cmd.append("-DCMAKE_CXX_COMPILER=" + os.environ.get("CXX", "clang++"))
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["ninja", "-C", str(BUILD)], check=True, stdout=subprocess.DEVNULL)


def find_tests(selected):
    tests = []
    roots = [pathlib.Path(s).resolve() for s in selected] if selected else [ROOT / "tests"] + DOC_DIRS
    for root in roots:
        if root.is_file():
            files = [root]
        elif root.exists():
            files = sorted(root.rglob("*.calx")) + sorted(root.rglob("*.md"))
        else:
            files = []
        for f in files:
            rel = f.relative_to(ROOT).as_posix()
            if f.suffix == ".calx":
                tests.append(Test(rel, f.read_text(encoding="utf-8"), f))
            elif any(d in f.parents for d in DOC_DIRS):
                tests.extend(doc_examples(f, rel))
    return tests


def doc_examples(path, rel):
    """Every ```calx block in a docs page is a test. Other fences (```text, ```sh) aren't."""
    out = []
    lines = path.read_text(encoding="utf-8").splitlines()
    i = 0
    while i < len(lines):
        if lines[i].strip() == "```calx":
            start = i + 1
            j = start
            while j < len(lines) and lines[j].strip() != "```":
                j += 1
            # Blank lines first, so error line numbers in the example match the page.
            source = "\n" * start + "\n".join(lines[start:j]) + "\n"
            out.append(Test(f"{rel}:{start + 1}", source))
            i = j
        i += 1
    return out


def run_test(test, mode):
    with tempfile.TemporaryDirectory() as tmp:
        if test.files:
            for name, text in test.files:
                f = pathlib.Path(tmp) / name
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_text(text, encoding="utf-8")
            path = pathlib.Path(tmp) / test.files[0][0]
            shown = test.name
        elif test.path:
            path = test.path
            shown = test.name
        else:
            path = pathlib.Path(tmp) / "example.calx"
            path.write_text(test.source, encoding="utf-8")
            shown = str(path)
        if test.errors:
            return check_errors(test, path)
        cmd = [str(CALX), "run", str(path)]
        if mode == "release":
            cmd.insert(2, "--release")
        if test.args:
            cmd += ["--", *test.args]
        # Bytes, so no newline is translated on the way in or out.
        stdin = "".join(line + "\n" for line in test.stdin).encode("utf-8")
        p = run_limited(cmd, stdin)
        if p is None:
            return [f"still running after {RUN_SECONDS} seconds, so it was stopped"]
        p.stdout = p.stdout.decode("utf-8", errors="replace")
        p.stderr = p.stderr.decode("utf-8", errors="replace")
        problems = []
        if test.panic:
            if p.returncode != 101:
                problems.append(f"expected a panic (status 101), got status {p.returncode}")
            m = next((PANIC.match(l) for l in p.stderr.splitlines() if PANIC.match(l)), None)
            if not m:
                problems.append("no panic message on stderr")
            else:
                if int(m.group(2)) != test.panic[0]:
                    problems.append(f"panicked on line {m.group(2)}, expected line {test.panic[0]}")
                if test.panic[1] not in m.group(4):
                    problems.append(f"panic message {m.group(4)!r} doesn't contain {test.panic[1]!r}")
            if test.trace:
                frames = [f"{f.group(1)}:{f.group(3)}" for f in map(FRAME.match, p.stderr.splitlines()) if f]
                if frames != test.trace:
                    problems.append("stack trace " + diff(test.trace, frames))
        elif p.returncode != test.status:
            problems.append(f"exit status {p.returncode}" + (f", expected {test.status}" if test.status else ""))
        got = p.stdout.splitlines()
        if got != test.out:
            problems.append(diff(test.out, got))
        if not test.panic and p.returncode == test.status and p.stderr.splitlines() != test.stderr:
            problems.append("standard error " + diff(test.stderr, p.stderr.splitlines()))
        elif problems and p.stderr.strip() and (not test.panic or p.returncode != 101):
            problems.append("stderr:\n" + indent(p.stderr.rstrip()))
        return problems


def run_limited(cmd, stdin):
    """Runs `cmd`, or returns None if it outlasts RUN_SECONDS. `calx run` starts the program as
    its own child, so the whole tree is stopped: the program would otherwise keep the pipes open."""
    group = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}
    with subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          cwd=ROOT, **group) as p:
        try:
            out, err = p.communicate(stdin, timeout=RUN_SECONDS)
        except subprocess.TimeoutExpired:
            if os.name == "nt":
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], capture_output=True)
            else:
                os.killpg(p.pid, signal.SIGKILL)
            p.communicate()
            return None
        return subprocess.CompletedProcess(cmd, p.returncode, out, err)


def check_errors(test, path):
    p = subprocess.run([str(CALX), "check", str(path)], capture_output=True, text=True, encoding="utf-8",
                       errors="replace", cwd=ROOT)
    problems = []
    if p.returncode == 0:
        return ["expected compile errors, but it compiled"]
    actual = []
    columns = []
    for line in p.stderr.splitlines():
        m = DIAG.match(line)
        if m:
            actual.append((int(m.group(2)), m.group(4)))
            columns.append((int(m.group(2)), int(m.group(3)), m.group(4)))
    problems += check_json(test, path, columns)
    remaining = list(actual)
    for line, text in test.errors:
        match = next((a for a in remaining if a[0] == line and text in a[1]), None)
        if match:
            remaining.remove(match)
        else:
            on_line = [a[1] for a in actual if a[0] == line]
            problems.append(f"line {line}: expected an error containing {text!r}" +
                            (f"; got {on_line}" if on_line else "; got none there"))
    for line, text in remaining:
        problems.append(f"line {line}: unexpected error: {text}")
    return problems


def check_json(test, path, expected):
    """`calx check --json --stdin` must report the errors the text output does, at the same places
    (docs/reference/command.md). `expected` holds (line, byte column, message), both from 1."""
    source = path.read_bytes()
    p = subprocess.run([str(CALX), "check", "--json", "--stdin", str(path)], input=source, capture_output=True, cwd=ROOT)
    problems = []
    if p.returncode != 1:
        problems.append(f"--json: exit status {p.returncode}, expected 1")
    lines = test.source.splitlines() if test.files else source.decode("utf-8", errors="replace").splitlines()
    got = []
    for raw in p.stdout.decode("utf-8", errors="replace").splitlines():
        try:
            d = json.loads(raw)
            start, end = d["range"]["start"], d["range"]["end"]
            if d["severity"] != "error":
                continue
            line, character = start["line"] + 1, start["character"]
        except (ValueError, KeyError, TypeError):
            problems.append(f"--json printed a line that isn't a diagnostic: {raw!r}")
            continue
        if (end["line"], end["character"]) < (start["line"], start["character"]):
            problems.append(f"--json: the range of {d['message']!r} ends before it starts")
        same = (path.resolve().parent in pathlib.Path(d["file"]).resolve().parents) if test.files else d["file"] == str(path)
        if not same:
            problems.append(f"--json: {d['message']!r} is in {d['file']!r}, expected {str(path)!r}")
        got.append((line, character, d["message"]))
    # Columns agree where a line is ASCII: editors count UTF-16 units from 0, the text form bytes from 1.
    want = []
    for line, col, message in expected:
        text = lines[line - 1] if 0 < line <= len(lines) else ""
        want.append((line, col - 1 if text.isascii() else None, message))
    got = [(l, c if (0 < l <= len(lines) and lines[l - 1].isascii()) else None, m) for l, c, m in got]
    if sorted(got, key=str) != sorted(want, key=str):
        problems.append("--json reports different errors from the text output:\n" +
                        indent("text: " + repr(sorted(want, key=str)) + "\njson: " + repr(sorted(got, key=str))))
    return problems


def diff(want, got):
    lines = ["output differs:"]
    for i in range(max(len(want), len(got))):
        w = want[i] if i < len(want) else "<nothing>"
        g = got[i] if i < len(got) else "<nothing>"
        mark = "  " if w == g else "! "
        lines.append(f"  {mark}line {i + 1}: expected {w!r}" + ("" if w == g else f", got {g!r}"))
    return "\n".join(lines)


def indent(text):
    return "\n".join("    " + l for l in text.splitlines())


def main():
    # A failing test's output can hold any text, which a Windows console's code page may not.
    sys.stdout.reconfigure(errors="backslashreplace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", help="test files or folders (default: all)")
    ap.add_argument("--modes", default="debug", help="comma-separated build modes: debug, release")
    ap.add_argument("--no-build", action="store_true", help="don't rebuild the compiler first")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    if not args.no_build:
        build_compiler()
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    tests = find_tests(args.paths)
    jobs = [(t, m) for t in tests for m in (modes if not t.errors else modes[:1])]
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda tm: (tm, run_test(*tm)), jobs))
    for (test, mode), problems in results:
        if problems:
            failed += 1
            print(f"FAIL {test.name} [{mode}]")
            for p in problems:
                print(indent(p))
    passed = len(jobs) - failed
    print(f"{passed} passed, {failed} failed ({len(tests)} tests, modes: {', '.join(modes)}, "
          f"{platform.system()} {platform.machine()})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
