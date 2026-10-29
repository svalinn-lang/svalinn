public class MatMul {
    public static void main(String[] args) {
        int n = 500; double[] a = new double[n * n], b = new double[n * n], c = new double[n * n];
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) { a[i * n + j] = ((i * 7 + j * 3) % 13) / 13.0; b[i * n + j] = ((i * 5 + j * 11) % 17) / 17.0; }
        for (int rep = 0; rep < 3; rep++) {
            java.util.Arrays.fill(c, 0.0);
            for (int i = 0; i < n; i++) for (int k = 0; k < n; k++) { double aik = a[i * n + k]; for (int j = 0; j < n; j++) c[i * n + j] += aik * b[k * n + j]; }
        }
        double sum = 0; for (double x : c) sum += x;
        System.out.printf("%.6f%n", sum);
    }
}
