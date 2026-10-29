import math
def a(i, j): return 1.0 / ((i + j) * (i + j + 1) // 2 + i + 1)
def mul_av(x, y):
    n = len(x)
    for i in range(n):
        s = 0.0
        for j in range(n): s += a(i, j) * x[j]
        y[i] = s
def mul_atv(x, y):
    n = len(x)
    for i in range(n):
        s = 0.0
        for j in range(n): s += a(j, i) * x[j]
        y[i] = s
n = 2000
u = [1.0] * n; v = [0.0] * n; t = [0.0] * n
for i in range(10):
    mul_av(u, t); mul_atv(t, v); mul_av(v, t); mul_atv(t, u)
vbv = vv = 0.0
for i in range(n):
    vbv += u[i] * v[i]; vv += v[i] * v[i]
print("%.9f" % math.sqrt(vbv / vv))
