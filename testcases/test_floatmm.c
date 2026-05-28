#include <stdio.h>
#include <stdlib.h>

/* ── triple-nested loop, depth 3, all-float arithmetic ─────── */
/*    Pure number-crunching: should be flagged VECTORIZABLE.    */
void matmul(const double *A, const double *B, double *C, int N) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            double s = 0.0;
            for (int k = 0; k < N; k++) {
                s += A[i*N + k] * B[k*N + j];   /* depth-3 hot spot */
            }
            C[i*N + j] = s;
        }
    }
}

/* ── element-wise transform, depth 1 ──────────────────────── */
void scale_rows(double *M, int N, double factor) {
    for (int i = 0; i < N * N; i++)
        M[i] = M[i] * factor + 1.0;
}

int main(void) {
    enum { N = 4 };
    double A[N*N], B[N*N], C[N*N];
    for (int i = 0; i < N*N; i++) {
        A[i] = (double)(i + 1);
        B[i] = (double)(N*N - i);
        C[i] = 0.0;
    }

    matmul(A, B, C, N);
    scale_rows(C, N, 0.5);

    double sum = 0.0;
    for (int i = 0; i < N*N; i++) sum += C[i];
    printf("checksum = %.2f\n", sum);
    return 0;
}
