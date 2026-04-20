#include <stdio.h>
#include <stdlib.h>

/* ── arithmetic heavy ── */
int compute(int a, int b) {
    int sum     = a + b;          /* add */
    int product = a * b;          /* mul */
    int diff    = a - b;          /* sub (treated as add-group) */
    int result  = sum + product + diff;
    return result;
}

/* ── loop + memory heavy ── */
void matrix_add(int *A, int *B, int *C, int n) {
    for (int i = 0; i < n; i++) {          /* outer loop */
        for (int j = 0; j < n; j++) {      /* inner loop — depth 2 */
            C[i*n + j] = A[i*n + j] + B[i*n + j];  /* load x2, add, store */
        }
    }
}

/* ── branch + compare heavy ── */
int classify(int x) {
    if (x < 0)        return -1;   /* icmp + branch */
    else if (x == 0)  return  0;
    else if (x < 100) return  1;
    else              return  2;
}

/* ── cast + mixed ── */
double scale(int x, float factor) {
    double dx  = (double)x;        /* cast: sitofp */
    double df  = (double)factor;   /* cast: fpext  */
    return dx * df;                /* fmul */
}

/* ── call heavy ── */
void process(int *data, int n) {
    for (int i = 0; i < n; i++) {
        data[i] = compute(data[i], i);     /* direct call inside loop */
        data[i] = classify(data[i]);       /* another call */
    }
}

int main() {
    int A[4] = {1, 2, 3, 4};
    int B[4] = {5, 6, 7, 8};
    int C[4] = {0};

    matrix_add(A, B, C, 2);
    process(C, 4);

    for (int i = 0; i < 4; i++)
        printf("C[%d] = %d\n", i, C[i]);   /* call: printf */

    double s = scale(42, 3.14f);
    printf("scale = %.2f\n", s);

    return 0;
}
