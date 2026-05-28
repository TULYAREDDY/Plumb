#include <stdio.h>
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
    const char *text = "Hello, World! 42 lines\n";
    int counts[6] = {0};
    for (int i = 0; text[i]; i++)
        counts[categorize(text[i])]++;

    for (int k = 0; k < 6; k++)
        printf("cat[%d]=%d\n", k, counts[k]);

    printf("parse_int(\"-1234\") = %d\n", parse_int("-1234"));
    return 0;
}
