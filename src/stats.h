/*
 * stats.h - the small amount of robust statistics the tool needs.
 * Pure functions, no global state; covered by tests/test_stats.c.
 */
#ifndef UA_STATS_H
#define UA_STATS_H

#include <stddef.h>
#include <stdint.h>

void ua_sort(double *a, size_t n);

/* `sorted` must be ascending.  p in [0,1]; linear interpolation. */
double ua_quantile_sorted(const double *sorted, size_t n, double p);

/* These copy their input; n == 0 returns NaN. */
double ua_median(const double *a, size_t n);
double ua_min(const double *a, size_t n);
double ua_max(const double *a, size_t n);
/* Median absolute deviation from the median (unscaled). */
double ua_mad(const double *a, size_t n);

/* Most frequent value; ties resolve to the smallest value.  *count receives
 * its multiplicity.  n == 0 returns 0 with *count = 0. */
int64_t ua_mode_i64(const int64_t *a, size_t n, size_t *count);

/* Least-squares line y = a + b*x.  Returns 0 on success, -1 if degenerate. */
int ua_linfit(const double *x, const double *y, size_t n, double *a, double *b);

/*
 * Step detection for capacity experiments: y is roughly `lo` while x is below
 * a capacity and roughly `hi` above it.
 *
 *   found     1 if hi/lo contrast reached `min_ratio`
 *   last_lo   largest x whose y is still below the midpoint
 *   first_hi  next x after it (the capacity lies in (last_lo, first_hi])
 *   lo, hi    plateau estimates (10th / 90th percentile of y)
 *
 * Noise in these experiments only ever adds time, so the scan runs from the
 * right: one slow outlier on the low plateau cannot move the result.
 */
typedef struct {
    int found;
    double last_lo;
    double first_hi;
    double lo;
    double hi;
} ua_step;

ua_step ua_find_step(const double *x, const double *y, size_t n, double min_ratio);

/* Round to the nearest multiple of `q` (q > 0). */
double ua_round_to(double v, double q);

#endif
