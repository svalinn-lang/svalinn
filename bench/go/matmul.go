package main

import "fmt"

func main() {
	n := 500
	a := make([]float64, n*n)
	b := make([]float64, n*n)
	c := make([]float64, n*n)
	for i := 0; i < n; i++ {
		for j := 0; j < n; j++ {
			a[i*n+j] = float64((i*7+j*3)%13) / 13.0
			b[i*n+j] = float64((i*5+j*11)%17) / 17.0
		}
	}
	for rep := 0; rep < 3; rep++ {
		for i := range c {
			c[i] = 0
		}
		for i := 0; i < n; i++ {
			for k := 0; k < n; k++ {
				aik := a[i*n+k]
				for j := 0; j < n; j++ {
					c[i*n+j] += aik * b[k*n+j]
				}
			}
		}
	}
	sum := 0.0
	for _, x := range c {
		sum += x
	}
	fmt.Printf("%.6f\n", sum)
}
