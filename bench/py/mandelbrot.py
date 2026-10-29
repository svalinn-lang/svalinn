def main():
    n = 4000
    count = 0
    for y in range(n):
        ci = 2.0 * y / n - 1.0
        for x in range(n):
            cr = 2.0 * x / n - 1.5
            zr = zi = 0.0
            inside = True
            for i in range(50):
                tr = zr * zr - zi * zi + cr
                ti = 2.0 * zr * zi + ci
                zr = tr; zi = ti
                if zr * zr + zi * zi > 4.0:
                    inside = False
                    break
            if inside: count += 1
    print(count)
main()
