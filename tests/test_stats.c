/* Unit and randomised tests for src/stats.c. */
#include "check.h"
#include "stats.h"

#include <string.h>

#define SEED 0x5eed1234abcdef01ull

static void test_basic(void)
{
    double a[] = {5, 1, 4, 2, 3};
    CHECK_NEAR(ua_median(a, 5), 3, 0);
    CHECK_NEAR(ua_min(a, 5), 1, 0);
    CHECK_NEAR(ua_max(a, 5), 5, 0);
    CHECK_NEAR(a[0], 5, 0); /* input untouched */
    double b[] = {1, 2, 3, 4};
    CHECK_NEAR(ua_median(b, 4), 2.5, 1e-12);
    CHECK(isnan(ua_median(b, 0)));
    CHECK(isnan(ua_min(b, 0)));
    CHECK(isnan(ua_mad(b, 0)));
    double s[] = {10, 20, 30, 40, 50};
    CHECK_NEAR(ua_quantile_sorted(s, 5, 0.0), 10, 0);
    CHECK_NEAR(ua_quantile_sorted(s, 5, 1.0), 50, 0);
    CHECK_NEAR(ua_quantile_sorted(s, 5, 0.25), 20, 1e-12);
    CHECK_NEAR(ua_quantile_sorted(s, 5, 0.9), 46, 1e-9);
    CHECK_NEAR(ua_quantile_sorted(s, 1, 0.5), 10, 0);
    double m[] = {1, 1, 2, 2, 4, 6, 9};
    CHECK_NEAR(ua_mad(m, 7), 1, 1e-12); /* median 2; deviations 1,1,0,0,2,4,7 */
    CHECK_NEAR(ua_round_to(7.26, 0.5), 7.5, 1e-12);
}

static void test_mode(void)
{
    int64_t a[] = {7, 3, 7, 3, 9};
    size_t n = 0;
    CHECK(ua_mode_i64(a, 5, &n) == 3); /* tie: smaller value */
    CHECK(n == 2);
    int64_t b[] = {4};
    CHECK(ua_mode_i64(b, 1, &n) == 4 && n == 1);
    CHECK(ua_mode_i64(b, 0, &n) == 0 && n == 0);
}

static void test_linfit(void)
{
    double x[] = {0, 1, 2, 3}, y[] = {1, 3, 5, 7}, a, b;
    CHECK(ua_linfit(x, y, 4, &a, &b) == 0);
    CHECK_NEAR(a, 1, 1e-12);
    CHECK_NEAR(b, 2, 1e-12);
    double same[] = {2, 2, 2};
    CHECK(ua_linfit(same, y, 3, &a, &b) == -1);
    CHECK(ua_linfit(x, y, 1, &a, &b) == -1);
}

/* Property: for any permutation-invariant statistic, shuffling the input
 * does not change the result; the median lies between min and max; at least
 * half of the values are <= the median and at least half are >= it. */
static void test_properties(void)
{
    unsigned long long s = SEED;
    for (int trial = 0; trial < 300; trial++) {
        size_t n = 1 + (size_t)(test_rand(&s) % 40);
        double a[40], b[40];
        for (size_t i = 0; i < n; i++)
            a[i] = (double)(test_rand(&s) % 1000) / 7.0;
        memcpy(b, a, sizeof a);
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = (size_t)(test_rand(&s) % (i + 1));
            double t = b[i];
            b[i] = b[j];
            b[j] = t;
        }
        double med = ua_median(a, n);
        CHECK_NEAR(med, ua_median(b, n), 1e-12);
        CHECK_NEAR(ua_mad(a, n), ua_mad(b, n), 1e-12);
        CHECK(med >= ua_min(a, n) && med <= ua_max(a, n));
        size_t le = 0, ge = 0;
        for (size_t i = 0; i < n; i++) {
            le += a[i] <= med;
            ge += a[i] >= med;
        }
        CHECK(2 * le >= n && 2 * ge >= n);
        ua_sort(b, n);
        for (size_t i = 1; i < n; i++)
            CHECK(b[i - 1] <= b[i]);
        if (g_failures) {
            fprintf(stderr, "seed %#llx trial %d\n", SEED, trial);
            return;
        }
    }
}

/* Property: a noisy step is located within one grid point of the truth, and
 * a single outlier on the low plateau does not move it. */
static void test_step(void)
{
    unsigned long long s = SEED ^ 0x1111;
    for (int trial = 0; trial < 200; trial++) {
        size_t n = 12 + (size_t)(test_rand(&s) % 30);
        size_t k = 3 + (size_t)(test_rand(&s) % (n - 6)); /* first high index, >= 3 high points */
        if (k > n - 3)
            k = n - 3;
        double x[64], y[64];
        for (size_t i = 0; i < n; i++) {
            x[i] = 8 * pow(1.25, (double)i);
            double noise = ((double)(test_rand(&s) % 1000) / 1000.0 - 0.5) * 0.08;
            y[i] = (i < k ? 1.0 : 1.8 - 0.01 * (double)(i - k)) + noise;
        }
        ua_step st = ua_find_step(x, y, n, 1.3);
        CHECK(st.found);
        CHECK_NEAR(st.first_hi, x[k], 1e-9);
        CHECK_NEAR(st.last_lo, x[k - 1], 1e-9);
        /* One slow outlier well before the step must not be taken for it. */
        if (k >= 5) {
            double saved = y[1];
            y[1] = 2.1;
            ua_step st2 = ua_find_step(x, y, n, 1.3);
            CHECK(st2.found);
            CHECK_NEAR(st2.first_hi, x[k], 1e-9);
            y[1] = saved;
        }
        if (g_failures) {
            fprintf(stderr, "seed %#llx trial %d (n=%zu k=%zu)\n", SEED ^ 0x1111, trial, n, k);
            return;
        }
    }
    /* No step: flat data, too few points, or everything high. */
    double fx[] = {1, 2, 3, 4, 5, 6}, fy[] = {1, 1.02, 0.99, 1.01, 1.0, 1.03};
    CHECK(!ua_find_step(fx, fy, 6, 1.3).found);
    CHECK(!ua_find_step(fx, fy, 3, 1.3).found);
    double hy[] = {2, 2, 2, 2, 2, 2};
    CHECK(!ua_find_step(fx, hy, 6, 1.3).found);
}

/* Steady states: the measured loop settles in one of a few states per run
 * (DESIGN.md, "Steady states"); the fastest one must be found exactly. */
static void test_fastest_state(void)
{
    size_t idx[64];
    /* Two states 10 % apart, mixed: only the fast one. */
    double two[] = {110.1, 100.2, 110.0, 100.0, 109.9, 100.1, 110.2, 100.3, 110.0};
    CHECK(ua_fastest_state(two, 9, 0.01, idx) == 4);
    for (int i = 0; i < 4; i++)
        CHECK(two[idx[i]] < 101);
    /* A lone fast outlier is not a state. */
    double lone[] = {90.0, 100.0, 100.4, 100.2, 107.0};
    CHECK(ua_fastest_state(lone, 5, 0.01, idx) == 3);
    for (int i = 0; i < 3; i++)
        CHECK(lone[idx[i]] >= 100.0 && lone[idx[i]] <= 100.4);
    /* Two outliers that are not within 1 % of each other either. */
    double two_lone[] = {80.0, 90.0, 100.0, 100.5, 100.9, 101.5};
    CHECK(ua_fastest_state(two_lone, 6, 0.01, idx) == 3);
    /* No three values agree: everything, as a plain median would. */
    double spread[] = {100, 103, 106, 109, 112};
    CHECK(ua_fastest_state(spread, 5, 0.01, idx) == 5);
    CHECK(ua_fastest_state(spread, 2, 0.01, idx) == 2);
    CHECK(ua_fastest_state(spread, 0, 0.01, idx) == 0);
    /* A single state is taken whole. */
    double one[] = {50.0, 50.1, 50.05, 50.2, 49.98};
    CHECK(ua_fastest_state(one, 5, 0.01, idx) == 5);

    /* Property: any mixture of a fast state (at least three runs) and slower
     * states 3 to 20 % above it, in any order, yields exactly the fast runs. */
    unsigned long long st = SEED ^ 0x5157;
    for (int trial = 0; trial < 500; trial++) {
        size_t n = 3 + (size_t)(test_rand(&st) % 60), nfast = 3 + (size_t)(test_rand(&st) % (n - 2));
        if (nfast > n)
            nfast = n;
        double base = 1000.0 + (double)(test_rand(&st) % 100000), a[64];
        for (size_t i = 0; i < n; i++) {
            double jitter = (double)(test_rand(&st) % 1000) / 1000.0 * 0.004 * base;
            double slow = i < nfast ? 0 : (0.03 + (double)(test_rand(&st) % 170) / 1000.0) * base;
            a[i] = base + jitter + slow;
        }
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = (size_t)(test_rand(&st) % (i + 1));
            double t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
        size_t m = ua_fastest_state(a, n, 0.01, idx);
        CHECK(m == nfast);
        for (size_t i = 0; i < m; i++)
            CHECK(a[idx[i]] < base * 1.03);
        if (g_failures) {
            fprintf(stderr, "seed %#llx trial %d (n=%zu fast=%zu)\n", SEED ^ 0x5157, trial, n, nfast);
            return;
        }
    }
}

int main(void)
{
    test_basic();
    test_mode();
    test_linfit();
    test_properties();
    test_step();
    test_fastest_state();
    return test_finish("test_stats");
}
