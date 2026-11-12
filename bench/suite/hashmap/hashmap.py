# Hash maps: 2 million integer keys inserted, 4 million looked up, half of them missing, every
# other key removed and all looked up again, and the values summed; then 500,000 string keys
# inserted and a million looked up. Each language uses its standard map with its default hash.
import array
import sys


def main():
    folder = sys.argv[1]
    ints = array.array("q")
    with open(f"{folder}/ints.bin", "rb") as f:
        ints.frombytes(f.read())
    n = int(sys.argv[2])
    m = {}
    for i in range(n):
        m[ints[i]] = i
    hits = total = 0
    for k in ints[: 2 * n]:
        v = m.get(k)
        if v is not None:
            hits += 1
            total += v
    print(f"inserted {len(m)}\nfound {hits} {total}")
    for i in range(0, n, 2):
        del m[ints[i]]
    hits = sum(1 for k in ints[:n] if k in m)
    print(f"left {len(m)} {hits} {sum(m.values())}")

    with open(f"{folder}/strings.txt", encoding="utf-8") as f:
        strings = f.read().splitlines()
    s = {}
    for i in range(len(strings) // 2):
        s[strings[i]] = i
    hits = total = 0
    for w in strings:
        v = s.get(w)
        if v is not None:
            hits += 1
            total += v
    print(f"strings {len(s)} {hits} {total}")


main()
