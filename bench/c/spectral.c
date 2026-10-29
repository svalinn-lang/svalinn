#include <stdio.h>
#include <math.h>
static double u[2000], v[2000], tmp[2000];
static double a(long i, long j) { return 1.0 / ((i + j) * (i + j + 1) / 2 + i + 1); }
static void mulAv(long n, double *x, double *y) { for (long i = 0; i < n; i++) { double s = 0; for (long j = 0; j < n; j++) s += a(i, j) * x[j]; y[i] = s; } }
static void mulAtv(long n, double *x, double *y) { for (long i = 0; i < n; i++) { double s = 0; for (long j = 0; j < n; j++) s += a(j, i) * x[j]; y[i] = s; } }
static void mulAtAv(long n, double *x, double *y) { mulAv(n, x, tmp); mulAtv(n, tmp, y); }
int main(void) {
    long n = 2000;
    for (long i = 0; i < n; i++) u[i] = 1;
    for (int i = 0; i < 10; i++) { mulAtAv(n, u, v); mulAtAv(n, v, u); }
    double vbv = 0, vv = 0;
    for (long i = 0; i < n; i++) { vbv += u[i] * v[i]; vv += v[i] * v[i]; }
    printf("%.9f\n", sqrt(vbv / vv));
    return 0;
}
