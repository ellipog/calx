# bench/text/text.calx in Python.
import time

text = "".join(f"  {i}, name {i} , {i * 7}, some more text here \n" for i in range(200000))
loops = join = replace = float("inf")
total = 0
for _ in range(7):
    t0 = time.perf_counter()
    fields = 0
    for line in text.splitlines():
        for c in line.split(","):
            fields += len(c.strip())
    t1 = time.perf_counter()
    joined = "|".join(text.splitlines())
    t2 = time.perf_counter()
    replaced = joined.replace("name", "NAME")
    t3 = time.perf_counter()
    total += fields + len(replaced)
    loops, join, replace = min(loops, t1 - t0), min(join, t2 - t1), min(replace, t3 - t2)
us = lambda s: round(s * 1e6)
print(f"python: fields {us(loops)} us, lines joined {us(join)} us, replace {us(replace)} us, total {total}")
