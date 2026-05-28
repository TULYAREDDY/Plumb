/*
 * test_memheavy.c — memory-bound workload
 * ─────────────────────────────────────────
 * Designed to stress the `memory` cost class. Every inner loop iteration
 * performs ≥ 4 loads and ≥ 1 store, with no opportunity for register
 * promotion across iterations (writes alias the same arrays the next
 * iteration reads). The pass should classify this as memory-dominated
 * and surface the inner accumulator block on the critical path.
 *
 * Expected analysis fingerprint:
 *   - `memory` group dominates total cost (>= 60 % at default weights)
 *   - HOTSPOT on stencil() and main()
 *   - Loop depth = 2 inside stencil(), so memory weight (=3) is multiplied
 *     by 2 → effective per-instruction cost of 6 in the inner body
 *   - VECTORIZABLE on stencil() (loop with no calls inside)
 */

#include <stdio.h>
#include <stdlib.h>

#define N 32

/* 1-D 5-point stencil; touches 5 distinct memory locations per iteration */
void stencil(const double *in, double *out, int n) {
    for (int t = 0; t < 4; t++) {                  /* outer "time step" loop */
        for (int i = 2; i < n - 2; i++) {          /* inner stencil sweep   */
            out[i] = 0.2 * (in[i-2] + in[i-1] + in[i] + in[i+1] + in[i+2]);
        }
    }
}

/* Pointer-chasing reduction; deliberately defeats vectorization */
double reduce_indirect(const double *buf, const int *idx, int n) {
    double s = 0.0;
    for (int i = 0; i < n; i++) {
        s += buf[idx[i]];                          /* load idx[i], load buf[..] */
    }
    return s;
}

int main(void) {
    double a[N], b[N];
    int    idx[N];

    for (int i = 0; i < N; i++) { a[i] = (double)i; b[i] = 0.0; idx[i] = (i * 7) % N; }

    stencil(a, b, N);
    double r = reduce_indirect(b, idx, N);
    printf("reduce = %.3f\n", r);
    return 0;
}
