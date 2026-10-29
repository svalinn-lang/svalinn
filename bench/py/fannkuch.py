def main():
    n = 10
    perm = [0] * 16; perm1 = list(range(16)); count = [0] * 16
    max_flips = checksum = perm_count = 0
    r = n
    while True:
        while r != 1:
            count[r - 1] = r; r -= 1
        perm[:n] = perm1[:n]
        flips = 0; k = perm[0]
        while k != 0:
            i, j = 0, k
            while i < j:
                perm[i], perm[j] = perm[j], perm[i]; i += 1; j -= 1
            flips += 1; k = perm[0]
        if flips > max_flips: max_flips = flips
        checksum += flips if perm_count % 2 == 0 else -flips
        done = True
        while r != n:
            p0 = perm1[0]
            for i in range(r): perm1[i] = perm1[i + 1]
            perm1[r] = p0
            count[r] -= 1
            if count[r] > 0:
                done = False; break
            r += 1
        if done: break
        perm_count += 1
    print(checksum); print(max_flips)
main()
