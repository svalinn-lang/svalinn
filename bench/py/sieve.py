def main():
    n = 20000000
    count = 0
    for rep in range(5):
        flags = bytearray([1]) * (n + 1)
        count = 0
        i = 2
        while i <= n:
            if flags[i]:
                count += 1
                j = i * i
                while j <= n:
                    flags[j] = 0
                    j += i
            i += 1
    print(count)
main()
