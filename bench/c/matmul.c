#include <stdio.h>
static double a[250000], b[250000], c[250000];
int main(void) {
    long n = 500;
    for (long i = 0; i < n; i++) for (long j = 0; j < n; j++) { a[i * n + j] = ((i * 7 + j * 3) % 13) / 13.0; b[i * n + j] = ((i * 5 + j * 11) % 17) / 17.0; }
    for (int rep = 0; rep < 3; rep++) {
        for (long i = 0; i < n * n; i++) c[i] = 0;
        for (long i = 0; i < n; i++) for (long k = 0; k < n; k++) { double aik = a[i * n + k]; for (long j = 0; j < n; j++) c[i * n + j] += aik * b[k * n + j]; }
    }
    double sum = 0; for (long i = 0; i < n * n; i++) sum += c[i];
    printf("%.6f\n", sum);
    return 0;
}
