package main

import "fmt"

type Node struct{ left, right *Node }

func make_(d int) *Node {
	if d > 0 {
		return &Node{make_(d - 1), make_(d - 1)}
	}
	return &Node{}
}
func check(n *Node) int64 {
	if n.left == nil {
		return 1
	}
	return 1 + check(n.left) + check(n.right)
}
func main() {
	maxDepth := 18
	fmt.Println(check(make_(maxDepth + 1)))
	longLived := make_(maxDepth)
	for d := 4; d <= maxDepth; d += 2 {
		iters := 1 << (maxDepth - d + 4)
		var total int64
		for i := 0; i < iters; i++ {
			total += check(make_(d))
		}
		fmt.Println(total)
	}
	fmt.Println(check(longLived))
}
