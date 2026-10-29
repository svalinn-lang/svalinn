public class Sieve {
    public static void main(String[] a) {
        int n = 20000000; boolean[] flags = new boolean[n + 1]; long count = 0;
        for (int rep = 0; rep < 5; rep++) {
            java.util.Arrays.fill(flags, true);
            count = 0;
            for (long i = 2; i <= n; i++) if (flags[(int) i]) { count++; for (long j = i * i; j <= n; j += i) flags[(int) j] = false; }
        }
        System.out.println(count);
    }
}
