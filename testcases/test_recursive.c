#include <stdio.h>

/* ── classic recursion: branchy + self-call ────────────────── */
long fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

/* ── tail-style recursion ─────────────────────────────────── */
long fact(int n) {
    if (n <= 1) return 1;
    return (long)n * fact(n - 1);
}

/* ── small leaf function (inline candidate) ───────────────── */
int square(int x) { return x * x; }

/* ── caller: demonstrates recursion + leaf call in a loop ── */
int main(void) {
    long sumF = 0;
    for (int i = 0; i < 8; i++)
        sumF += fib(i) + square(i);

    long f = fact(10);
    printf("sumF=%ld fact(10)=%ld\n", sumF, f);
    return 0;
}
