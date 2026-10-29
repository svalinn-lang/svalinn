public class NBody {
    static final double SOLAR_MASS = 39.47841760435743, DAYS = 365.24;
    static double[] x = new double[5], y = new double[5], z = new double[5], vx = new double[5], vy = new double[5], vz = new double[5], m = new double[5];
    static void set(int i, double a, double b, double c, double d, double e, double f, double g) { x[i] = a; y[i] = b; z[i] = c; vx[i] = d * DAYS; vy[i] = e * DAYS; vz[i] = f * DAYS; m[i] = g * SOLAR_MASS; }
    static double energy() {
        double e = 0;
        for (int i = 0; i < 5; i++) {
            e += 0.5 * m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
            for (int j = i + 1; j < 5; j++) { double dx = x[i] - x[j], dy = y[i] - y[j], dz = z[i] - z[j]; e -= m[i] * m[j] / Math.sqrt(dx * dx + dy * dy + dz * dz); }
        }
        return e;
    }
    static void advance(double dt) {
        for (int i = 0; i < 5; i++) for (int j = i + 1; j < 5; j++) {
            double dx = x[i] - x[j], dy = y[i] - y[j], dz = z[i] - z[j], d2 = dx * dx + dy * dy + dz * dz, mag = dt / (d2 * Math.sqrt(d2));
            double mj = m[j] * mag, mi = m[i] * mag;
            vx[i] -= dx * mj; vy[i] -= dy * mj; vz[i] -= dz * mj; vx[j] += dx * mi; vy[j] += dy * mi; vz[j] += dz * mi;
        }
        for (int i = 0; i < 5; i++) { x[i] += dt * vx[i]; y[i] += dt * vy[i]; z[i] += dt * vz[i]; }
    }
    public static void main(String[] a) {
        set(0, 0, 0, 0, 0, 0, 0, 1);
        set(1, 4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01, 1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05, 9.54791938424326609e-04);
        set(2, 8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01, -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05, 2.85885980666130812e-04);
        set(3, 1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01, 2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05, 4.36624404335156298e-05);
        set(4, 1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01, 2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05, 5.15138902046611451e-05);
        double px = 0, py = 0, pz = 0;
        for (int i = 0; i < 5; i++) { px += vx[i] * m[i]; py += vy[i] * m[i]; pz += vz[i] * m[i]; }
        vx[0] = -px / SOLAR_MASS; vy[0] = -py / SOLAR_MASS; vz[0] = -pz / SOLAR_MASS;
        System.out.printf("%.9f%n", energy());
        for (int s = 0; s < 5000000; s++) advance(0.01);
        System.out.printf("%.9f%n", energy());
    }
}
