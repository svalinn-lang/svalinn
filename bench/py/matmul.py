def main():
    n = 500
    a = [0.0] * (n * n); b = [0.0] * (n * n); c = [0.0] * (n * n)
    for i in range(n):
        for j in range(n):
            a[i * n + j] = ((i * 7 + j * 3) % 13) / 13.0
            b[i * n + j] = ((i * 5 + j * 11) % 17) / 17.0
    for rep in range(3):
        for i in range(n * n): c[i] = 0.0
        for i in range(n):
            for k in range(n):
                aik = a[i * n + k]
                for j in range(n): c[i * n + j] += aik * b[k * n + j]
    s = 0.0
    for x in c: s += x
    print("%.6f" % s)
main()
