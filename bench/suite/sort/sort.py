# Sorting: 5 million 64-bit integers and 5 million doubles read from binary files, and a million
# strings read a line each, each list sorted with the language's own sort and eleven evenly
# spaced elements printed.
import array
import sys


def main():
    folder = sys.argv[1]
    ints = array.array("q")
    with open(f"{folder}/ints.bin", "rb") as f:
        ints.frombytes(f.read())
    floats = array.array("d")
    with open(f"{folder}/floats.bin", "rb") as f:
        floats.frombytes(f.read())
    with open(f"{folder}/strings.txt", encoding="utf-8") as f:
        strings = f.read().splitlines()

    ints = sorted(ints)
    floats = sorted(floats)
    strings.sort()

    for k in range(11):
        i = min(k * len(ints) // 10, len(ints) - 1)
        j = min(k * len(floats) // 10, len(floats) - 1)
        s = min(k * len(strings) // 10, len(strings) - 1)
        print(f"{ints[i]} {floats[j]:.6f} {strings[s]}")


main()
