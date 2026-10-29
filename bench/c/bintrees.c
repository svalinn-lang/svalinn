#include <stdio.h>
#include <stdlib.h>
typedef struct Node { struct Node *left, *right; } Node;
static Node *make(int d) { Node *n = malloc(16); if (d > 0) { n->left = make(d - 1); n->right = make(d - 1); } else n->left = n->right = 0; return n; }
static long check(Node *n) { return n->left ? 1 + check(n->left) + check(n->right) : 1; }
static void release(Node *n) { if (n->left) { release(n->left); release(n->right); } free(n); }
int main(void) {
    int maxDepth = 18;
    Node *s = make(maxDepth + 1); printf("%ld\n", check(s)); release(s);
    Node *longLived = make(maxDepth);
    for (int d = 4; d <= maxDepth; d += 2) {
        long iters = 1L << (maxDepth - d + 4), total = 0;
        for (long i = 0; i < iters; i++) { Node *t = make(d); total += check(t); release(t); }
        printf("%ld\n", total);
    }
    printf("%ld\n", check(longLived)); release(longLived);
    return 0;
}
