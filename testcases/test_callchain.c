/*
 * test_callchain.c — call-bound workload + indirect-call surcharge
 * ─────────────────────────────────────────────────────────────────
 * Designed to stress the `call` cost class and the indirect-call ×1.6
 * surcharge applied by the pass. Mixes:
 *   - a chain of small leaf functions (direct calls)
 *   - one dispatch through a function pointer (indirect call)
 *
 * Expected analysis fingerprint:
 *   - `call` group dominates dispatch() cost
 *   - dispatch() has at least one indirect call → its `call` cost
 *     should be 1.6 × the equivalent direct-call cost
 *   - leaf functions (add1/mul2/neg) are flagged INLINE_CANDIDATE
 *   - run_pipeline() is HOTSPOT because every loop iteration calls dispatch()
 */

#include <stdio.h>

/* ── tiny leaves: each should be flagged INLINE_CANDIDATE ─────── */
static int add1(int x) { return x + 1; }
static int mul2(int x) { return x * 2; }
static int neg (int x) { return -x;     }

/* ── direct-call pipeline ─────────────────────────────────────── */
static int pipeline_direct(int x) {
    return neg(mul2(add1(x)));            /* 3 direct calls */
}

/* ── indirect dispatch via function pointer ──────────────────── */
typedef int (*op_t)(int);

static int dispatch(op_t op, int x) {
    return op(x);                          /* INDIRECT call: ×1.6 surcharge */
}

/* ── main hot loop: lots of calls, both kinds ────────────────── */
int run_pipeline(const int *in, int *out, int n) {
    int sum = 0;
    op_t ops[3] = { add1, mul2, neg };
    for (int i = 0; i < n; i++) {
        int v = pipeline_direct(in[i]);    /* direct chain */
        v     = dispatch(ops[i % 3], v);   /* indirect    */
        out[i] = v;
        sum += v;
    }
    return sum;
}

int main(void) {
    int in[8], out[8];
    for (int i = 0; i < 8; i++) in[i] = i;

    int s = run_pipeline(in, out, 8);
    printf("checksum = %d\n", s);
    return 0;
}
