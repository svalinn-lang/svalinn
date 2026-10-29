package main

import (
	"fmt"
	"math"
)

func a(i, j int) float64 { return 1.0 / float64((i+j)*(i+j+1)/2+i+1) }
func mulAv(x, y []float64) {
	for i := range y {
		s := 0.0
		for j := range x {
			s += a(i, j) * x[j]
		}
		y[i] = s
	}
}
func mulAtv(x, y []float64) {
	for i := range y {
		s := 0.0
		for j := range x {
			s += a(j, i) * x[j]
		}
		y[i] = s
	}
}
func main() {
	n := 2000
	u := make([]float64, n)
	v := make([]float64, n)
	t := make([]float64, n)
	for i := range u {
		u[i] = 1
	}
	for i := 0; i < 10; i++ {
		mulAv(u, t)
		mulAtv(t, v)
		mulAv(v, t)
		mulAtv(t, u)
	}
	vbv, vv := 0.0, 0.0
	for i := 0; i < n; i++ {
		vbv += u[i] * v[i]
		vv += v[i] * v[i]
	}
	fmt.Printf("%.9f\n", math.Sqrt(vbv/vv))
}
