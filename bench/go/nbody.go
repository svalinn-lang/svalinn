package main

import (
	"fmt"
	"math"
)

type Body struct{ x, y, z, vx, vy, vz, mass float64 }

const solarMass = 39.47841760435743
const days = 365.24

var bodies [5]Body

func set(i int, x, y, z, vx, vy, vz, m float64) {
	bodies[i] = Body{x, y, z, vx * days, vy * days, vz * days, m * solarMass}
}
func energy() float64 {
	e := 0.0
	for i := 0; i < 5; i++ {
		b := &bodies[i]
		e += 0.5 * b.mass * (b.vx*b.vx + b.vy*b.vy + b.vz*b.vz)
		for j := i + 1; j < 5; j++ {
			dx := bodies[i].x - bodies[j].x
			dy := bodies[i].y - bodies[j].y
			dz := bodies[i].z - bodies[j].z
			e -= bodies[i].mass * bodies[j].mass / math.Sqrt(dx*dx+dy*dy+dz*dz)
		}
	}
	return e
}
func advance(dt float64) {
	for i := 0; i < 5; i++ {
		for j := i + 1; j < 5; j++ {
			dx := bodies[i].x - bodies[j].x
			dy := bodies[i].y - bodies[j].y
			dz := bodies[i].z - bodies[j].z
			d2 := dx*dx + dy*dy + dz*dz
			mag := dt / (d2 * math.Sqrt(d2))
			mj := bodies[j].mass * mag
			mi := bodies[i].mass * mag
			bodies[i].vx -= dx * mj
			bodies[i].vy -= dy * mj
			bodies[i].vz -= dz * mj
			bodies[j].vx += dx * mi
			bodies[j].vy += dy * mi
			bodies[j].vz += dz * mi
		}
	}
	for i := 0; i < 5; i++ {
		bodies[i].x += dt * bodies[i].vx
		bodies[i].y += dt * bodies[i].vy
		bodies[i].z += dt * bodies[i].vz
	}
}
func main() {
	set(0, 0, 0, 0, 0, 0, 0, 1)
	set(1, 4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01, 1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05, 9.54791938424326609e-04)
	set(2, 8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01, -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05, 2.85885980666130812e-04)
	set(3, 1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01, 2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05, 4.36624404335156298e-05)
	set(4, 1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01, 2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05, 5.15138902046611451e-05)
	px, py, pz := 0.0, 0.0, 0.0
	for i := 0; i < 5; i++ {
		px += bodies[i].vx * bodies[i].mass
		py += bodies[i].vy * bodies[i].mass
		pz += bodies[i].vz * bodies[i].mass
	}
	bodies[0].vx = -px / solarMass
	bodies[0].vy = -py / solarMass
	bodies[0].vz = -pz / solarMass
	fmt.Printf("%.9f\n", energy())
	for s := 0; s < 5000000; s++ {
		advance(0.01)
	}
	fmt.Printf("%.9f\n", energy())
}
