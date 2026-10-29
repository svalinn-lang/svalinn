public class Spectral {
    static double a(int i, int j) { return 1.0 / ((i + j) * (i + j + 1) / 2 + i + 1); }
    static void mulAv(double[] x, double[] y) { int n = x.length; for (int i = 0; i < n; i++) { double s = 0; for (int j = 0; j < n; j++) s += a(i, j) * x[j]; y[i] = s; } }
    static void mulAtv(double[] x, double[] y) { int n = x.length; for (int i = 0; i < n; i++) { double s = 0; for (int j = 0; j < n; j++) s += a(j, i) * x[j]; y[i] = s; } }
    public static void main(String[] args) {
        int n = 2000; double[] u = new double[n], v = new double[n], t = new double[n];
        java.util.Arrays.fill(u, 1.0);
        for (int i = 0; i < 10; i++) { mulAv(u, t); mulAtv(t, v); mulAv(v, t); mulAtv(t, u); }
        double vbv = 0, vv = 0;
        for (int i = 0; i < n; i++) { vbv += u[i] * v[i]; vv += v[i] * v[i]; }
        System.out.printf("%.9f%n", Math.sqrt(vbv / vv));
    }
}
