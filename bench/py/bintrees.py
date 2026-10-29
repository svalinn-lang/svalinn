def make(d): return (make(d - 1), make(d - 1)) if d > 0 else (None, None)
def check(n): return 1 if n[0] is None else 1 + check(n[0]) + check(n[1])
max_depth = 18
print(check(make(max_depth + 1)))
long_lived = make(max_depth)
for d in range(4, max_depth + 1, 2):
    iters = 1 << (max_depth - d + 4)
    total = 0
    for i in range(iters): total += check(make(d))
    print(total)
print(check(long_lived))
