# Many files: 2,000 files of 1 to 16 KB written into 20 folders, read back and their bytes added
# up, then removed with their folders, through the language's own file functions.
import os
import sys


def main():
    root = f"{sys.argv[1]}/run"
    files = int(sys.argv[2])
    pattern = bytes(j % 251 for j in range(16384 + 251))
    os.mkdir(root)
    for d in range(20):
        os.mkdir(f"{root}/d{d}")
    written = 0
    for i in range(files):
        size = 1024 + (i * 7919) % 15000
        start = i % 251
        with open(f"{root}/d{i % 20}/f{i}.bin", "wb") as f:
            f.write(pattern[start : start + size])
        written += size
    total = read = 0
    for i in range(files):
        with open(f"{root}/d{i % 20}/f{i}.bin", "rb") as f:
            data = f.read()
        read += len(data)
        total += sum(data)
    for i in range(files):
        os.remove(f"{root}/d{i % 20}/f{i}.bin")
    for d in range(20):
        os.rmdir(f"{root}/d{d}")
    os.rmdir(root)
    print(f"files {files}\nwritten {written}\nread {read}\nsum {total}")


main()
