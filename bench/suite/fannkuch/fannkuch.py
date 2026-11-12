# fannkuch-redux: the Benchmarks Game's pancake flips over every permutation of 1..n, in the order
# its counting algorithm makes them, with the checksum and the most flips any permutation took.
import sys


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 7
    perm1 = list(range(n))
    count = [0] * n
    max_flips = checksum = perm_count = 0
    r = n
    while True:
        while r != 1:
            count[r - 1] = r
            r -= 1
        perm = perm1[:]
        flips = 0
        k = perm[0]
        while k:
            perm[: k + 1] = perm[k::-1]
            flips += 1
            k = perm[0]
        max_flips = max(max_flips, flips)
        checksum += flips if perm_count % 2 == 0 else -flips
        while True:
            if r == n:
                print(f"{checksum}\nPfannkuchen({n}) = {max_flips}")
                return
            perm0 = perm1[0]
            perm1[:r] = perm1[1 : r + 1]
            perm1[r] = perm0
            count[r] -= 1
            if count[r] > 0:
                break
            r += 1
        perm_count += 1


main()
