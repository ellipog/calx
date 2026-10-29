"""Checks std.text.utf8.decode_lossy and std.io.Lines against Java 25 on random bytes.

    py -I tools/unicode/utf8_check.py [--java PATH] [--seed N]

Each run builds tools/unicode/utf8_check.calx, then gives it and Utf8Check.java the same input:
random byte strings, one per line in hex, to decode, and random streams of text with every kind
of line end, bytes that aren't UTF-8 and characters cut off, to split into lines. Calx gets each
stream in random pieces, so lines end at every place a read can, as `\\r` before a `\\n` that
comes in the next read.
"""

import argparse
import os
import pathlib
import random
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
CALX = ROOT / "build" / "compiler" / ("windows/calx.exe" if os.name == "nt" else "linux/calx")

# Bytes where the rules change: continuations at their ends, every kind of lead, and bytes that
# never appear in UTF-8.
EDGES = [0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xE1, 0xEC, 0xED,
         0xEE, 0xEF, 0xF0, 0xF1, 0xF3, 0xF4, 0xF5, 0xF7, 0xF8, 0xFF]


def piece(rng, ends):
    r = rng.random()
    if r < 0.25:
        return bytes([rng.choice(EDGES)])
    if r < 0.35:
        return bytes([rng.randrange(0x80, 0xC0)])
    if r < 0.6:
        c = rng.choice([rng.randrange(0x80, 0x800), rng.randrange(0x800, 0x10000), rng.randrange(0x10000, 0x110000)])
        if 0xD800 <= c <= 0xDFFF:
            # A surrogate, as UTF-8 can't hold it, with its three bytes as Java decodes them.
            return bytes([0xED, 0xA0 | ((c >> 6) & 0x1F), 0x80 | (c & 0x3F)])
        b = chr(c).encode("utf-8")
        # Cut off, or whole.
        return b[:rng.randrange(1, len(b))] if rng.random() < 0.3 else b
    if ends and r < 0.75:
        return rng.choice([b"\n", b"\r", b"\r\n", b"\n\r"])
    return bytes([rng.randrange(0x20, 0x7F)])


def run(cmd, data):
    p = subprocess.run(cmd, input=data, capture_output=True)
    if p.returncode != 0:
        sys.exit(f"{cmd[0]} failed:\n{p.stderr.decode(errors='replace')}")
    return p.stdout.decode().splitlines()


def run_in_pieces(cmd, data, rng):
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    i = 0
    while i < len(data):
        n = rng.choice([1, 1, 2, 3, 7, 50, 4000])
        p.stdin.write(data[i:i + n])
        p.stdin.flush()
        i += n
        if rng.random() < 0.3:
            time.sleep(0.001)
    p.stdin.close()
    out = p.stdout.read()
    err = p.stderr.read()
    if p.wait() != 0:
        sys.exit(f"{cmd[0]} failed:\n{err.decode(errors='replace')}")
    return out.decode().splitlines()


def compare(what, want, got):
    if want == got:
        return True
    for k in range(max(len(want), len(got))):
        w = want[k] if k < len(want) else "(nothing)"
        g = got[k] if k < len(got) else "(nothing)"
        if w != g:
            print(f"{what}: line {k + 1}: Java gives {w}, Calx {g}")
            return False
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--java", default=str(pathlib.Path(os.environ.get("JAVA_HOME", "")) / "bin" / "java"))
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / ("utf8_check.exe" if os.name == "nt" else "utf8_check")
        subprocess.run([str(CALX), "build", "--release", str(HERE / "utf8_check.calx"), "-o", str(exe)], check=True)
        java = [a.java, str(HERE / "Utf8Check.java")]

        strings = []
        for _ in range(200000):
            strings.append(b"".join(piece(rng, False) for _ in range(rng.randrange(0, 8))))
        data = "".join(s.hex() + "\n" for s in strings).encode()
        ok = compare("decode", run(java + ["decode"], data), run([str(exe), "decode"], data))
        print(f"decode: {len(strings)} strings")

        lines = 0
        for k in range(40):
            stream = b"".join(piece(rng, True) for _ in range(rng.randrange(0, 4000)))
            want = run(java + ["lines"], stream)
            got = run_in_pieces([str(exe), "lines"], stream, rng)
            ok = compare(f"stream {k + 1}", want, got) and ok
            lines += len(want)
        print(f"lines: 40 streams, {lines} lines")
    print("same as Java" if ok else "different from Java")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
