// n-body: the Benchmarks Game's model of the Jovian planets, advanced n steps of 0.01 with plain
// doubles, printing the system's energy before and after. Every version in this folder does the
// same arithmetic in the same order.
package main

import (
	"fmt"
	"math"
	"os"
	"strconv"
)

const (
	solarMass   = 4 * math.Pi * math.Pi
	daysPerYear = 365.24
)

type Body struct {
	x, y, z, vx, vy, vz, mass float64
}

func planet(x, y, z, vx, vy, vz, mass float64) Body {
	return Body{x, y, z, vx * daysPerYear, vy * daysPerYear, vz * daysPerYear, mass * solarMass}
}

func advance(bodies []Body, dt float64) {
	for i := range bodies {
		a := &bodies[i]
		for j := i + 1; j < len(bodies); j++ {
			b := &bodies[j]
			dx := a.x - b.x
			dy := a.y - b.y
			dz := a.z - b.z
			d2 := dx*dx + dy*dy + dz*dz
			mag := dt / (d2 * math.Sqrt(d2))
			mi := a.mass * mag
			mj := b.mass * mag
			a.vx -= dx * mj
			a.vy -= dy * mj
			a.vz -= dz * mj
			b.vx += dx * mi
			b.vy += dy * mi
			b.vz += dz * mi
		}
	}
	for i := range bodies {
		b := &bodies[i]
		b.x += dt * b.vx
		b.y += dt * b.vy
		b.z += dt * b.vz
	}
}

func energy(bodies []Body) float64 {
	e := 0.0
	for i, b := range bodies {
		e += 0.5 * b.mass * (b.vx*b.vx + b.vy*b.vy + b.vz*b.vz)
		for _, c := range bodies[i+1:] {
			dx := b.x - c.x
			dy := b.y - c.y
			dz := b.z - c.z
			e -= b.mass * c.mass / math.Sqrt(dx*dx+dy*dy+dz*dz)
		}
	}
	return e
}

func offsetMomentum(bodies []Body) {
	var px, py, pz float64
	for _, b := range bodies {
		px += b.vx * b.mass
		py += b.vy * b.mass
		pz += b.vz * b.mass
	}
	bodies[0].vx = -px / solarMass
	bodies[0].vy = -py / solarMass
	bodies[0].vz = -pz / solarMass
}

func main() {
	n := 1000
	if len(os.Args) > 1 {
		n, _ = strconv.Atoi(os.Args[1])
	}
	bodies := []Body{
		{0, 0, 0, 0, 0, 0, solarMass},
		planet(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01, 1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05, 9.54791938424326609e-04),
		planet(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01, -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05, 2.85885980666130812e-04),
		planet(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01, 2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05, 4.36624404335156298e-05),
		planet(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01, 2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05, 5.15138902046611451e-05),
	}
	offsetMomentum(bodies)
	fmt.Printf("%.9f\n", energy(bodies))
	for k := 0; k < n; k++ {
		advance(bodies, 0.01)
	}
	fmt.Printf("%.9f\n", energy(bodies))
}
