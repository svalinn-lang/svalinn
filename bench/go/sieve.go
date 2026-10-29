package main

import "fmt"

func main() {
	n := 20000000
	flags := make([]bool, n+1)
	count := 0
	for rep := 0; rep < 5; rep++ {
		for i := range flags {
			flags[i] = true
		}
		count = 0
		for i := 2; i <= n; i++ {
			if flags[i] {
				count++
				for j := i * i; j <= n; j += i {
					flags[j] = false
				}
			}
		}
	}
	fmt.Println(count)
}
