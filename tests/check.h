/* Minimal test helpers shared by the C tests. */
#ifndef UA_TEST_CHECK_H
#define UA_TEST_CHECK_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                          \
    do {                                                                               \
        double a_ = (a), b_ = (b);                                                     \
        g_checks++;                                                                    \
        if (!(fabs(a_ - b_) <= (eps))) {                                               \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: %s = %.9g, expected %.9g\n", __FILE__, __LINE__,   \
                    #a, a_, b_);                                                       \
        }                                                                              \
    } while (0)

/* xorshift64: deterministic randomness for property tests. */
static inline unsigned long long test_rand(unsigned long long *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static inline int test_finish(const char *name)
{
    if (g_failures) {
        fprintf(stderr, "%s: %d of %d checks FAILED\n", name, g_failures, g_checks);
        return 1;
    }
    printf("%s: %d checks passed\n", name, g_checks);
    return 0;
}

#endif
