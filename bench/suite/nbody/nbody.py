# n-body: the Benchmarks Game's model of the Jovian planets, advanced n steps of 0.01 with plain
# doubles, printing the system's energy before and after. Every version in this folder does the
# same arithmetic in the same order.
import math
import sys

PI = 3.141592653589793
SOLAR_MASS = 4.0 * PI * PI
DAYS_PER_YEAR = 365.24


class Body:
    __slots__ = ("x", "y", "z", "vx", "vy", "vz", "mass")

    def __init__(self, x, y, z, vx, vy, vz, mass):
        self.x, self.y, self.z = x, y, z
        self.vx, self.vy, self.vz = vx, vy, vz
        self.mass = mass


def planet(x, y, z, vx, vy, vz, mass):
    return Body(x, y, z, vx * DAYS_PER_YEAR, vy * DAYS_PER_YEAR, vz * DAYS_PER_YEAR, mass * SOLAR_MASS)


def advance(bodies, dt):
    n = len(bodies)
    for i in range(n):
        a = bodies[i]
        for j in range(i + 1, n):
            b = bodies[j]
            dx = a.x - b.x
            dy = a.y - b.y
            dz = a.z - b.z
            d2 = dx * dx + dy * dy + dz * dz
            mag = dt / (d2 * math.sqrt(d2))
            mi = a.mass * mag
            mj = b.mass * mag
            a.vx -= dx * mj
            a.vy -= dy * mj
            a.vz -= dz * mj
            b.vx += dx * mi
            b.vy += dy * mi
            b.vz += dz * mi
    for b in bodies:
        b.x += dt * b.vx
        b.y += dt * b.vy
        b.z += dt * b.vz


def energy(bodies):
    e = 0.0
    for i, b in enumerate(bodies):
        e += 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz)
        for c in bodies[i + 1:]:
            dx = b.x - c.x
            dy = b.y - c.y
            dz = b.z - c.z
            e -= b.mass * c.mass / math.sqrt(dx * dx + dy * dy + dz * dz)
    return e


def offset_momentum(bodies):
    px = py = pz = 0.0
    for b in bodies:
        px += b.vx * b.mass
        py += b.vy * b.mass
        pz += b.vz * b.mass
    bodies[0].vx = -px / SOLAR_MASS
    bodies[0].vy = -py / SOLAR_MASS
    bodies[0].vz = -pz / SOLAR_MASS


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
    bodies = [
        Body(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, SOLAR_MASS),
        planet(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01, 1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05, 9.54791938424326609e-04),
        planet(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01, -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05, 2.85885980666130812e-04),
        planet(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01, 2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05, 4.36624404335156298e-05),
        planet(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01, 2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05, 5.15138902046611451e-05),
    ]
    offset_momentum(bodies)
    print(f"{energy(bodies):.9f}")
    for _ in range(n):
        advance(bodies, 0.01)
    print(f"{energy(bodies):.9f}")


main()
