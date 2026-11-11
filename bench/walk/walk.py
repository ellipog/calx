"""bench/walk/walk.calx with Python's os.walk and glob.glob, each sorted as std.fs sorts."""
import glob
import os
import sys
import time


def main():
    root = sys.argv[1]
    if not os.path.isdir(os.path.join(root, "d19", "e19")):
        sys.exit("make the tree with walk.calx first")
    walked = globbed = float("inf")
    total = 0
    for _ in range(7):
        t0 = time.perf_counter()
        files = dirs = chars = 0
        for top, ds, fs in os.walk(root):
            ds.sort()
            for d in ds:
                dirs += 1
                chars += len(os.path.join(top, d))
            for f in sorted(fs):
                files += 1
                chars += len(os.path.join(top, f))
        t1 = time.perf_counter()
        found = 0
        for p in sorted(glob.glob(root + "/**/*.calx", recursive=True)):
            found += 1
            chars += len(p)
        t2 = time.perf_counter()
        total += files + dirs + found + chars
        walked = min(walked, t1 - t0)
        globbed = min(globbed, t2 - t1)
    print("python: walk %d us, glob %d us, total %d" % (walked * 1e6, globbed * 1e6, total))


main()
