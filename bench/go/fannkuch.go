package main

import "fmt"

func main() {
	n := 10
	var perm, perm1, count [16]int
	maxFlips, checksum, permCount := 0, 0, 0
	for i := 0; i < n; i++ {
		perm1[i] = i
	}
	r := n
	for {
		for r != 1 {
			count[r-1] = r
			r--
		}
		perm = perm1
		flips := 0
		for k := perm[0]; k != 0; k = perm[0] {
			for i, j := 0, k; i < j; i, j = i+1, j-1 {
				perm[i], perm[j] = perm[j], perm[i]
			}
			flips++
		}
		if flips > maxFlips {
			maxFlips = flips
		}
		if permCount%2 == 0 {
			checksum += flips
		} else {
			checksum -= flips
		}
		done := true
		for r != n {
			p0 := perm1[0]
			for i := 0; i < r; i++ {
				perm1[i] = perm1[i+1]
			}
			perm1[r] = p0
			count[r]--
			if count[r] > 0 {
				done = false
				break
			}
			r++
		}
		if done {
			break
		}
		permCount++
	}
	fmt.Printf("%d\n%d\n", checksum, maxFlips)
}
