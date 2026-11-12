# Word count: 5 million words of text, about 30 MB, lowercased, split into runs of ASCII letters,
# counted in the language's hash map, and the 20 most common printed, ties by the word. Python
# splits with re.findall, as Python does, and counts with a Counter.
import re
import sys
from collections import Counter


def main():
    with open(sys.argv[1], encoding="utf-8") as f:
        words = re.findall("[a-z]+", f.read().lower())
    counts = Counter(words)
    print(f"words {len(words)}\ndistinct {len(counts)}")
    for w, c in sorted(counts.items(), key=lambda e: (-e[1], e[0]))[:20]:
        print(f"{c} {w}")


main()
