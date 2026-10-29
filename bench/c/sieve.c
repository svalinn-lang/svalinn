#include <stdio.h>
#include <stdbool.h>
static bool flags[20000001];
int main(void) {
    long n = 20000000, count = 0;
    for (int rep = 0; rep < 5; rep++) {
        for (long i = 0; i < n + 1; i++) flags[i] = true;
        count = 0;
        for (long i = 2; i <= n; i++)
            if (flags[i]) { count++; for (long j = i * i; j <= n; j += i) flags[j] = false; }
    }
    printf("%ld\n", count);
    return 0;
}
