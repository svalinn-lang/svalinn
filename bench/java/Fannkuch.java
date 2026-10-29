public class Fannkuch {
    public static void main(String[] a) {
        int n = 10; int[] perm = new int[16], perm1 = new int[16], count = new int[16];
        int maxFlips = 0, checksum = 0, permCount = 0;
        for (int i = 0; i < n; i++) perm1[i] = i;
        int r = n;
        while (true) {
            while (r != 1) { count[r - 1] = r; r--; }
            System.arraycopy(perm1, 0, perm, 0, n);
            int flips = 0, k = perm[0];
            while (k != 0) { for (int i = 0, j = k; i < j; i++, j--) { int t = perm[i]; perm[i] = perm[j]; perm[j] = t; } flips++; k = perm[0]; }
            if (flips > maxFlips) maxFlips = flips;
            checksum += permCount % 2 == 0 ? flips : -flips;
            boolean done = true;
            while (r != n) {
                int p0 = perm1[0];
                for (int i = 0; i < r; i++) perm1[i] = perm1[i + 1];
                perm1[r] = p0;
                if (--count[r] > 0) { done = false; break; }
                r++;
            }
            if (done) break;
            permCount++;
        }
        System.out.println(checksum); System.out.println(maxFlips);
    }
}
