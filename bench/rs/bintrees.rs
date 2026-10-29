struct Node { left: Option<Box<Node>>, right: Option<Box<Node>> }
fn make(d: i32) -> Box<Node> { if d > 0 { Box::new(Node { left: Some(make(d - 1)), right: Some(make(d - 1)) }) } else { Box::new(Node { left: None, right: None }) } }
fn check(n: &Node) -> i64 { match (&n.left, &n.right) { (Some(l), Some(r)) => 1 + check(l) + check(r), _ => 1 } }
fn main() {
    let max_depth = 18;
    let s = make(max_depth + 1); println!("{}", check(&s)); drop(s);
    let long_lived = make(max_depth);
    let mut d = 4;
    while d <= max_depth {
        let iters = 1i64 << (max_depth - d + 4); let mut total = 0i64;
        for _ in 0..iters { let t = make(d); total += check(&t); }
        println!("{}", total); d += 2;
    }
    println!("{}", check(&long_lived));
}
