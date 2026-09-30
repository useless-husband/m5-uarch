#include "stats.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

void ua_sort(double *a, size_t n)
{
    if (n > 1)
        qsort(a, n, sizeof *a, cmp_double);
}

double ua_quantile_sorted(const double *sorted, size_t n, double p)
{
    if (n == 0)
        return NAN;
    if (p <= 0)
        return sorted[0];
    if (p >= 1)
        return sorted[n - 1];
    double pos = p * (double)(n - 1);
    size_t i = (size_t)pos;
    double frac = pos - (double)i;
    if (i + 1 >= n)
        return sorted[n - 1];
    return sorted[i] + frac * (sorted[i + 1] - sorted[i]);
}

static double *dup_sorted(const double *a, size_t n)
{
    double *c = malloc(n * sizeof *c);
    if (!c)
        return NULL;
    memcpy(c, a, n * sizeof *c);
    ua_sort(c, n);
    return c;
}

double ua_median(const double *a, size_t n)
{
    if (n == 0)
        return NAN;
    double *c = dup_sorted(a, n);
    if (!c)
        return NAN;
    double m = ua_quantile_sorted(c, n, 0.5);
    free(c);
    return m;
}

double ua_min(const double *a, size_t n)
{
    if (n == 0)
        return NAN;
    double m = a[0];
    for (size_t i = 1; i < n; i++)
        if (a[i] < m)
            m = a[i];
    return m;
}

double ua_max(const double *a, size_t n)
{
    if (n == 0)
        return NAN;
    double m = a[0];
    for (size_t i = 1; i < n; i++)
        if (a[i] > m)
            m = a[i];
    return m;
}

double ua_mad(const double *a, size_t n)
{
    if (n == 0)
        return NAN;
    double med = ua_median(a, n);
    double *d = malloc(n * sizeof *d);
    if (!d)
        return NAN;
    for (size_t i = 0; i < n; i++)
        d[i] = fabs(a[i] - med);
    double m = ua_median(d, n);
    free(d);
    return m;
}

int64_t ua_mode_i64(const int64_t *a, size_t n, size_t *count)
{
    if (n == 0) {
        if (count)
            *count = 0;
        return 0;
    }
    int64_t *c = malloc(n * sizeof *c);
    if (!c) {
        if (count)
            *count = 1;
        return a[0];
    }
    memcpy(c, a, n * sizeof *c);
    qsort(c, n, sizeof *c, cmp_i64);
    int64_t best = c[0];
    size_t best_n = 0, run = 0;
    for (size_t i = 0; i < n; i++) {
        run = (i > 0 && c[i] == c[i - 1]) ? run + 1 : 1;
        if (run > best_n) { /* strict: ties keep the smaller value */
            best_n = run;
            best = c[i];
        }
    }
    free(c);
    if (count)
        *count = best_n;
    return best;
}

int ua_linfit(const double *x, const double *y, size_t n, double *a, double *b)
{
    if (n < 2)
        return -1;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; i++) {
        sx += x[i];
        sy += y[i];
        sxx += x[i] * x[i];
        sxy += x[i] * y[i];
    }
    double den = (double)n * sxx - sx * sx;
    if (fabs(den) < 1e-300)
        return -1;
    *b = ((double)n * sxy - sx * sy) / den;
    *a = (sy - *b * sx) / (double)n;
    return 0;
}

ua_step ua_find_step(const double *x, const double *y, size_t n, double min_ratio)
{
    ua_step s = {0, NAN, NAN, NAN, NAN};
    if (n < 4)
        return s;
    double *c = dup_sorted(y, n);
    if (!c)
        return s;
    /* Low plateau: the lower quartile.  High plateau: the second largest
     * value, so that a capacity near the end of the scanned range (few high
     * points) is still found while one outlier is ignored. */
    s.lo = ua_quantile_sorted(c, n, 0.25);
    s.hi = c[n - 2];
    free(c);
    if (!(s.lo > 0) || s.hi / s.lo < min_ratio)
        return s;
    double mid = 0.5 * (s.lo + s.hi);
    /* The step is the first point at or above the midpoint that is followed
     * by more of the same: at least two of the next three points (as many
     * as exist) must be high as well, so that one slow outlier on the low
     * plateau is not mistaken for the step. */
    for (size_t i = 1; i < n; i++) {
        if (y[i] < mid)
            continue;
        size_t next = 0, high = 0;
        for (size_t k = i + 1; k < n && k <= i + 3; k++) {
            next++;
            high += y[k] >= mid;
        }
        if (next == 0 || high * 3 >= next * 2) {
            s.found = 1;
            s.last_lo = x[i - 1];
            s.first_hi = x[i];
            return s;
        }
    }
    return s;
}

double ua_round_to(double v, double q) { return q > 0 ? round(v / q) * q : v; }
