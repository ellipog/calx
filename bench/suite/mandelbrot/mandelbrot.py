# mandelbrot: the Benchmarks Game's n by n bitmap of the Mandelbrot set, 50 iterations a pixel,
# one bit a pixel, eight to a byte, as a PBM holds it. It prints the bitmap's FNV-1a hash rather
# than the bitmap.
import sys


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    bits = bytearray()
    size = float(n)
    for y in range(n):
        ci = 2.0 * y / size - 1.0
        acc = 0
        count = 0
        for x in range(n):
            cr = 2.0 * x / size - 1.5
            zr = zi = tr = ti = 0.0
            i = 0
            while i < 50 and tr + ti <= 4.0:
                zi = 2.0 * zr * zi + ci
                zr = tr - ti + cr
                tr = zr * zr
                ti = zi * zi
                i += 1
            acc <<= 1
            if tr + ti <= 4.0:
                acc |= 1
            count += 1
            if count == 8:
                bits.append(acc)
                acc = count = 0
            elif x == n - 1:
                bits.append((acc << (8 - n % 8)) & 0xFF)
                acc = count = 0
    h = 0xCBF29CE484222325
    for b in bits:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    print(f"P4 {n} {n}\nfnv1a {h:016x}")


main()
