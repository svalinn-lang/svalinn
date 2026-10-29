function make(d) { return d > 0 ? { left: make(d - 1), right: make(d - 1) } : { left: null, right: null }; }
function check(n) { return n.left === null ? 1 : 1 + check(n.left) + check(n.right); }
const maxDepth = 18;
console.log(check(make(maxDepth + 1)));
const longLived = make(maxDepth);
for (let d = 4; d <= maxDepth; d += 2) {
  const iters = 1 << (maxDepth - d + 4); let total = 0;
  for (let i = 0; i < iters; i++) total += check(make(d));
  console.log(total);
}
console.log(check(longLived));
