public class BinTrees {
    static final class Node { Node left, right; Node(Node l, Node r) { left = l; right = r; } }
    static Node make(int d) { return d > 0 ? new Node(make(d - 1), make(d - 1)) : new Node(null, null); }
    static long check(Node n) { return n.left == null ? 1 : 1 + check(n.left) + check(n.right); }
    public static void main(String[] a) {
        int maxDepth = 18;
        System.out.println(check(make(maxDepth + 1)));
        Node longLived = make(maxDepth);
        for (int d = 4; d <= maxDepth; d += 2) {
            long iters = 1L << (maxDepth - d + 4), total = 0;
            for (long i = 0; i < iters; i++) total += check(make(d));
            System.out.println(total);
        }
        System.out.println(check(longLived));
    }
}
