#include <stdio.h>
int main(void) {
    long n = 4000, count = 0;
    for (long y = 0; y < n; y++) {
        double ci = 2.0 * y / n - 1.0;
        for (long x = 0; x < n; x++) {
            double cr = 2.0 * x / n - 1.5, zr = 0, zi = 0; int inside = 1;
            for (int i = 0; i < 50; i++) {
                double tr = zr * zr - zi * zi + cr, ti = 2.0 * zr * zi + ci;
                zr = tr; zi = ti;
                if (zr * zr + zi * zi > 4.0) { inside = 0; break; }
            }
            count += inside;
        }
    }
    printf("%ld\n", count);
    return 0;
}
