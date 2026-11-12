# Building and taking apart text: a million CSV-like lines built onto one string with formatting,
# then each line split at its commas, a field uppercased and three joined with " | " onto a second
# string, the lines that name one user counted, and a word replaced through the whole result.
# Python builds each string from a list of parts with "".join, its quick way to put many together.
import sys


def main():
    n = int(sys.argv[1])
    words = ["alpha", "beta", "gamma", "delta", "epsilon"]
    built = "".join(f"{i},user{i % 1000},{i * 7 % 10007}.{i % 100:02d},{words[i % 5]}\n" for i in range(n))
    parts = []
    matches = 0
    for line in built.splitlines():
        fields = line.split(",")
        parts.append(f"{fields[3].upper()} | {fields[1]} | {fields[2]}\n")
        if fields[1] == "user42":
            matches += 1
    out = "".join(parts)
    replaced = out.replace("user", "u")
    print(f"built {len(built)}\nout {len(out)}\nmatches {matches}\nreplaced {len(replaced)}")


main()
