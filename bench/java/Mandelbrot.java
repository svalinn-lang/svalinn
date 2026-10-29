public class Mandelbrot {
    public static void main(String[] a) {
        int n = 4000; long count = 0;
        for (int y = 0; y < n; y++) {
            double ci = 2.0 * y / n - 1.0;
            for (int x = 0; x < n; x++) {
                double cr = 2.0 * x / n - 1.5, zr = 0, zi = 0; boolean inside = true;
                for (int i = 0; i < 50; i++) {
                    double tr = zr * zr - zi * zi + cr, ti = 2.0 * zr * zi + ci; zr = tr; zi = ti;
                    if (zr * zr + zi * zi > 4.0) { inside = false; break; }
                }
                if (inside) count++;
            }
        }
        System.out.println(count);
    }
}
