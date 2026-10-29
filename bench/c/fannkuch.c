#include <stdio.h>
int main(void) {
    int n = 10, perm[16], perm1[16], count[16], maxFlips = 0, checksum = 0, permCount = 0;
    for (int i = 0; i < n; i++) perm1[i] = i;
    int r = n;
    for (;;) {
        while (r != 1) { count[r - 1] = r; r--; }
        for (int i = 0; i < n; i++) perm[i] = perm1[i];
        int flips = 0, k = perm[0];
        while (k != 0) {
            for (int i = 0, j = k; i < j; i++, j--) { int t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
            flips++; k = perm[0];
        }
        if (flips > maxFlips) maxFlips = flips;
        checksum += permCount % 2 == 0 ? flips : -flips;
        int done = 1;
        while (r != n) {
            int p0 = perm1[0];
            for (int i = 0; i < r; i++) perm1[i] = perm1[i + 1];
            perm1[r] = p0;
            if (--count[r] > 0) { done = 0; break; }
            r++;
        }
        if (done) break;
        permCount++;
    }
    printf("%d\n%d\n", checksum, maxFlips);
    return 0;
}
