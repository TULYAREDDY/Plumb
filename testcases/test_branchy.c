#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── high cyclomatic complexity: switch + nested ifs ───────── */
int categorize(char c) {
    switch (c) {
        case 'a': case 'e': case 'i': case 'o': case 'u':
            return 1;                     /* vowel */
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return 2;                     /* digit */
        case ' ': case '\t': case '\n':
            return 3;                     /* whitespace */
        default:
            if (c >= 'A' && c <= 'Z') return 4;     /* upper */
            if (c >= 'a' && c <= 'z') return 5;     /* lower */
            return 0;                                /* other */
    }
}

/* ── HIGH_COMPLEXITY remedy: same classification, table-driven ──
 * `categorize`'s CC (23) comes from 18 case labels + 2 ifs. Collapsing
 * the switch to a precomputed lookup table removes the branching
 * entirely — every input takes the same single-load path. Later
 * designated-range initializers override earlier ones for overlapping
 * indices, so this list is ordered digit/space/upper/lower first and
 * vowels last, to reproduce categorize's exact vowel-beats-lower
 * precedence. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winitializer-overrides"
static const unsigned char CLASS_TABLE[256] = {
    ['0' ... '9'] = 2,
    [' ']  = 3, ['\t'] = 3, ['\n'] = 3,
    ['A' ... 'Z'] = 4,
    ['a' ... 'z'] = 5,
    ['a'] = 1, ['e'] = 1, ['i'] = 1, ['o'] = 1, ['u'] = 1,
};
#pragma clang diagnostic pop

int categorize_v2(char c) {
    return CLASS_TABLE[(unsigned char)c];
}

/* categorize_v2 must agree with categorize on every byte value — proves
 * the lower-complexity refactor is behavior-preserving, not just
 * structurally simpler. Kept as its own function (not inlined into
 * main's body) so this check doesn't change main's cost profile — the
 * numbers documented for `main` throughout EVALUATION.md are about the
 * program's original control flow, not this verification harness. */
static void check_categorize_equivalence(void) {
    for (int c = 0; c < 256; c++) {
        if (categorize((char)c) != categorize_v2((char)c)) {
            fprintf(stderr, "categorize/categorize_v2 mismatch at %d\n", c);
            abort();
        }
    }
}

/* ── branchy state machine ────────────────────────────────── */
int parse_int(const char *s) {
    int sign = 1, val = 0, i = 0;
    if (s[0] == '-') { sign = -1; i = 1; }
    else if (s[0] == '+') { i = 1; }

    while (s[i]) {
        if (s[i] < '0' || s[i] > '9') return 0;
        val = val * 10 + (s[i] - '0');
        i++;
    }
    return sign * val;
}

int main(void) {
    check_categorize_equivalence();

    const char *text = "Hello, World! 42 lines\n";
    int counts[6] = {0};
    for (int i = 0; text[i]; i++)
        counts[categorize(text[i])]++;

    for (int k = 0; k < 6; k++)
        printf("cat[%d]=%d\n", k, counts[k]);

    printf("parse_int(\"-1234\") = %d\n", parse_int("-1234"));
    return 0;
}
