package main

import "fmt"

func main() {
	n := 4000
	count := 0
	for y := 0; y < n; y++ {
		ci := 2.0*float64(y)/float64(n) - 1.0
		for x := 0; x < n; x++ {
			cr := 2.0*float64(x)/float64(n) - 1.5
			zr, zi := 0.0, 0.0
			inside := true
			for i := 0; i < 50; i++ {
				tr := zr*zr - zi*zi + cr
				ti := 2.0*zr*zi + ci
				zr, zi = tr, ti
				if zr*zr+zi*zi > 4.0 {
					inside = false
					break
				}
			}
			if inside {
				count++
			}
		}
	}
	fmt.Println(count)
}
