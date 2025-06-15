/*
 * stats.c — statistical functions for sheetcalc
 *
 * Copyright (c) 2024-2025 Sheetcalc Contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "sc_internal.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

static int extract_numbers(const ScVal *vals, size_t count, double **out, size_t *nout)
{
    double *buf;
    size_t i, n = 0, cap;
    double d;

    cap = count > 0 ? count : 1;
    buf = (double *)malloc(cap * sizeof(double));
    if (!buf)
        return SC_ERR_NOMEM;

    for (i = 0; i < count; i++) {
        if (sc_val_to_num(&vals[i], &d) != SC_OK)
            continue;
        if (isnan(d) || isinf(d))
            continue;
        if (n >= cap) {
            cap *= 2;
            {
                double *nb = (double *)realloc(buf, cap * sizeof(double));
                if (!nb) {
                    free(buf);
                    return SC_ERR_NOMEM;
                }
                buf = nb;
            }
        }
        buf[n++] = d;
    }

    *out = buf;
    *nout = n;
    return SC_OK;
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static void sort_doubles(double *arr, size_t n)
{
    if (n > 1)
        qsort(arr, n, sizeof(double), cmp_double);
}

static double interp_percentile(const double *sorted, size_t n, double pct)
{
    double rank, frac, lo, hi;
    size_t idx;

    if (n == 0)
        return 0.0;
    if (pct <= 0.0)
        return sorted[0];
    if (pct >= 100.0)
        return sorted[n - 1];

    rank = (pct / 100.0) * (double)(n - 1);
    idx = (size_t)floor(rank);
    frac = rank - (double)idx;

    if (idx + 1 >= n)
        return sorted[n - 1];

    lo = sorted[idx];
    hi = sorted[idx + 1];
    return lo + frac * (hi - lo);
}

static double compute_mean(const double *arr, size_t n)
{
    double sum = 0.0;
    size_t i;

    if (n == 0)
        return 0.0;
    for (i = 0; i < n; i++)
        sum += arr[i];
    return sum / (double)n;
}

static double compute_variance(const double *arr, size_t n, double mean, int sample)
{
    double acc = 0.0;
    size_t i;
    double denom;

    if (n == 0)
        return 0.0;
    denom = sample ? (double)(n - 1) : (double)n;
    if (denom <= 0.0)
        return 0.0;

    for (i = 0; i < n; i++) {
        double d = arr[i] - mean;
        acc += d * d;
    }
    return acc / denom;
}

static double compute_median(double *sorted, size_t n)
{
    if (n == 0)
        return 0.0;
    if (n % 2 == 1)
        return sorted[n / 2];
    return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
}

static double compute_mode(const double *arr, size_t n)
{
    double best = 0.0;
    int best_count = 0;
    size_t i, j;

    if (n == 0)
        return 0.0;

    for (i = 0; i < n; i++) {
        int cnt = 1;
        for (j = i + 1; j < n; j++) {
            if (arr[j] == arr[i])
                cnt++;
        }
        if (cnt > best_count) {
            best_count = cnt;
            best = arr[i];
        }
    }
    return best;
}

static double compute_skewness(const double *arr, size_t n, double mean, double stdev)
{
    double acc = 0.0;
    size_t i;

    if (n < 3 || stdev == 0.0)
        return 0.0;

    for (i = 0; i < n; i++) {
        double z = (arr[i] - mean) / stdev;
        acc += z * z * z;
    }
    return acc * (double)n / ((double)(n - 1) * (double)(n - 2));
}

static double compute_kurtosis(const double *arr, size_t n, double mean, double stdev)
{
    double acc = 0.0;
    size_t i;

    if (n < 4 || stdev == 0.0)
        return 0.0;

    for (i = 0; i < n; i++) {
        double z = (arr[i] - mean) / stdev;
        acc += z * z * z * z;
    }
    {
        double n_d = (double)n;
        return (n_d * (n_d + 1.0) / ((n_d - 1.0) * (n_d - 2.0) * (n_d - 3.0))) * acc
            - (3.0 * (n_d - 1.0) * (n_d - 1.0)) / ((n_d - 2.0) * (n_d - 3.0));
    }
}

int sci_stats_describe(ScVal *vals, size_t count, CsStatResult *out)
{
    double *nums = NULL;
    size_t n = 0;
    double mean, var, stdev;
    double min_v, max_v;
    size_t i;

    if (!vals || !out)
        return SC_ERR_INTERNAL;

    memset(out, 0, sizeof(*out));

    if (extract_numbers(vals, count, &nums, &n) != SC_OK)
        return SC_ERR_NOMEM;
    if (n == 0) {
        free(nums);
        return SC_ERR_TYPE;
    }

    sort_doubles(nums, n);
    mean = compute_mean(nums, n);
    var = compute_variance(nums, n, mean, 1);
    stdev = sqrt(var);

    min_v = nums[0];
    max_v = nums[n - 1];

    out->count = (int)n;
    out->mean = mean;
    out->median = compute_median(nums, n);
    out->mode = compute_mode(nums, n);
    out->variance = var;
    out->stdev = stdev;
    out->min = min_v;
    out->max = max_v;
    out->skewness = compute_skewness(nums, n, mean, stdev);
    out->kurtosis = compute_kurtosis(nums, n, mean, stdev);
    out->q1 = interp_percentile(nums, n, 25.0);
    out->q3 = interp_percentile(nums, n, 75.0);
    out->iqr = out->q3 - out->q1;
    out->sum = 0.0;
    for (i = 0; i < n; i++)
        out->sum += nums[i];

    free(nums);
    return SC_OK;
}

double sci_stats_percentile(ScVal *vals, size_t count, double pct)
{
    double *nums = NULL;
    size_t n = 0;
    double result;

    if (!vals || pct < 0.0 || pct > 100.0)
        return 0.0;

    if (extract_numbers(vals, count, &nums, &n) != SC_OK || n == 0) {
        free(nums);
        return 0.0;
    }

    sort_doubles(nums, n);
    result = interp_percentile(nums, n, pct);
    free(nums);
    return result;
}

double sci_stats_correlation(ScVal *xs, ScVal *ys, size_t count)
{
    double *xnums = NULL, *ynums = NULL;
    size_t nx = 0, ny = 0, n, i;
    double mean_x, mean_y, cov = 0.0, var_x = 0.0, var_y = 0.0;
    double denom;

    if (!xs || !ys || count == 0)
        return 0.0;

    if (extract_numbers(xs, count, &xnums, &nx) != SC_OK)
        return 0.0;
    if (extract_numbers(ys, count, &ynums, &ny) != SC_OK) {
        free(xnums);
        return 0.0;
    }

    n = nx < ny ? nx : ny;
    if (n < 2) {
        free(xnums);
        free(ynums);
        return 0.0;
    }

    mean_x = compute_mean(xnums, n);
    mean_y = compute_mean(ynums, n);

    for (i = 0; i < n; i++) {
        double dx = xnums[i] - mean_x;
        double dy = ynums[i] - mean_y;
        cov += dx * dy;
        var_x += dx * dx;
        var_y += dy * dy;
    }

    denom = sqrt(var_x * var_y);
    free(xnums);
    free(ynums);

    if (denom == 0.0)
        return 0.0;
    return cov / denom;
}

double sci_stats_covariance(ScVal *xs, ScVal *ys, size_t count, int sample)
{
    double *xnums = NULL, *ynums = NULL;
    size_t nx = 0, ny = 0, n, i;
    double mean_x, mean_y, cov = 0.0;
    double denom;

    if (!xs || !ys || count == 0)
        return 0.0;

    if (extract_numbers(xs, count, &xnums, &nx) != SC_OK)
        return 0.0;
    if (extract_numbers(ys, count, &ynums, &ny) != SC_OK) {
        free(xnums);
        return 0.0;
    }

    n = nx < ny ? nx : ny;
    if (n == 0) {
        free(xnums);
        free(ynums);
        return 0.0;
    }

    mean_x = compute_mean(xnums, n);
    mean_y = compute_mean(ynums, n);

    for (i = 0; i < n; i++)
        cov += (xnums[i] - mean_x) * (ynums[i] - mean_y);

    denom = sample ? (double)(n - 1) : (double)n;
    free(xnums);
    free(ynums);

    if (denom <= 0.0)
        return 0.0;
    return cov / denom;
}

int sci_stats_regression(ScVal *xs, ScVal *ys, size_t count,
                         double *slope, double *intercept, double *r2)
{
    double *xnums = NULL, *ynums = NULL;
    size_t nx = 0, ny = 0, n, i;
    double mean_x, mean_y;
    double ss_xx = 0.0, ss_xy = 0.0, ss_yy = 0.0;
    double m, b, r;

    if (!xs || !ys || !slope || !intercept || !r2)
        return SC_ERR_INTERNAL;

    if (extract_numbers(xs, count, &xnums, &nx) != SC_OK)
        return SC_ERR_NOMEM;
    if (extract_numbers(ys, count, &ynums, &ny) != SC_OK) {
        free(xnums);
        return SC_ERR_NOMEM;
    }

    n = nx < ny ? nx : ny;
    if (n < 2) {
        free(xnums);
        free(ynums);
        *slope = 0.0;
        *intercept = 0.0;
        *r2 = 0.0;
        return SC_ERR_TYPE;
    }

    mean_x = compute_mean(xnums, n);
    mean_y = compute_mean(ynums, n);

    for (i = 0; i < n; i++) {
        double dx = xnums[i] - mean_x;
        double dy = ynums[i] - mean_y;
        ss_xx += dx * dx;
        ss_xy += dx * dy;
        ss_yy += dy * dy;
    }

    if (ss_xx == 0.0) {
        free(xnums);
        free(ynums);
        *slope = 0.0;
        *intercept = mean_y;
        *r2 = 0.0;
        return SC_OK;
    }

    m = ss_xy / ss_xx;
    b = mean_y - m * mean_x;
    r = (ss_xx > 0.0 && ss_yy > 0.0) ? (ss_xy * ss_xy) / (ss_xx * ss_yy) : 0.0;

    *slope = m;
    *intercept = b;
    *r2 = r;

    free(xnums);
    free(ynums);
    return SC_OK;
}

int sci_stats_frequency(ScVal *vals, size_t count, int nbuckets, CsFreqBucket *out)
{
    double *nums = NULL;
    size_t n = 0, i;
    double min_v, max_v, width;
    int b;

    if (!vals || !out || nbuckets <= 0)
        return SC_ERR_INTERNAL;

    if (extract_numbers(vals, count, &nums, &n) != SC_OK)
        return SC_ERR_NOMEM;
    if (n == 0) {
        free(nums);
        return SC_ERR_TYPE;
    }

    sort_doubles(nums, n);
    min_v = nums[0];
    max_v = nums[n - 1];

    if (min_v == max_v) {
        width = 1.0;
    } else {
        width = (max_v - min_v) / (double)nbuckets;
        if (width == 0.0)
            width = 1.0;
    }

    for (b = 0; b < nbuckets; b++) {
        out[b].lo = min_v + width * (double)b;
        out[b].hi = (b == nbuckets - 1) ? max_v : min_v + width * (double)(b + 1);
        out[b].count = 0;
    }

    for (i = 0; i < n; i++) {
        int idx;
        if (width == 0.0 || min_v == max_v) {
            idx = 0;
        } else {
            idx = (int)((nums[i] - min_v) / width);
            if (idx >= nbuckets)
                idx = nbuckets - 1;
            if (idx < 0)
                idx = 0;
        }
        out[idx].count++;
    }

    free(nums);
    return SC_OK;
}

double sci_stats_mean(const ScVal *vals, size_t count)
{
    double *nums = NULL;
    size_t n = 0;
    double m;

    if (!vals)
        return 0.0;
    if (extract_numbers(vals, count, &nums, &n) != SC_OK || n == 0) {
        free(nums);
        return 0.0;
    }
    m = compute_mean(nums, n);
    free(nums);
    return m;
}

double sci_stats_stdev(const ScVal *vals, size_t count, int sample)
{
    double *nums = NULL;
    size_t n = 0;
    double mean, var;

    if (!vals)
        return 0.0;
    if (extract_numbers(vals, count, &nums, &n) != SC_OK || n == 0) {
        free(nums);
        return 0.0;
    }
    mean = compute_mean(nums, n);
    var = compute_variance(nums, n, mean, sample);
    free(nums);
    return sqrt(var);
}

int sci_stats_quartiles(ScVal *vals, size_t count, double *q1, double *q2,
                        double *q3)
{
    double *nums = NULL;
    size_t n = 0;

    if (!vals || !q1 || !q2 || !q3)
        return SC_ERR_INTERNAL;

    if (extract_numbers(vals, count, &nums, &n) != SC_OK)
        return SC_ERR_NOMEM;
    if (n == 0) {
        free(nums);
        return SC_ERR_TYPE;
    }

    sort_doubles(nums, n);
    *q1 = interp_percentile(nums, n, 25.0);
    *q2 = interp_percentile(nums, n, 50.0);
    *q3 = interp_percentile(nums, n, 75.0);
    free(nums);
    return SC_OK;
}

int sci_stats_range_from_sheet(ScSheet *sh, int r0, int c0, int r1, int c1,
                               CsStatResult *out)
{
    ScVal *vals;
    int nrows, ncols, r, c, idx, rc;

    if (!sh || !out)
        return SC_ERR_INTERNAL;
    if (r0 < 0 || c0 < 0 || r1 >= sh->nrows || c1 >= sh->ncols || r0 > r1 || c0 > c1)
        return SC_ERR_RANGE;

    nrows = r1 - r0 + 1;
    ncols = c1 - c0 + 1;
    vals = (ScVal *)calloc((size_t)nrows * (size_t)ncols, sizeof(ScVal));
    if (!vals)
        return SC_ERR_NOMEM;

    idx = 0;
    for (r = r0; r <= r1; r++) {
        for (c = c0; c <= c1; c++) {
            ScCell *cl;
            sc_val_nil(&vals[idx]);
            if (sci_cell_at(sh, r, c, &cl) == SC_OK)
                sc_val_copy(&vals[idx], &cl->value);
            idx++;
        }
    }

    rc = sci_stats_describe(vals, (size_t)idx, out);

    for (idx = 0; idx < nrows * ncols; idx++)
        sc_val_free(&vals[idx]);
    free(vals);
    return rc;
}

int sci_stats_zscore(ScVal *vals, size_t count, double *zout)
{
    double *nums = NULL;
    size_t n = 0, i;
    double mean, stdev;

    if (!vals || !zout)
        return SC_ERR_INTERNAL;

    if (extract_numbers(vals, count, &nums, &n) != SC_OK)
        return SC_ERR_NOMEM;
    if (n == 0) {
        free(nums);
        return SC_ERR_TYPE;
    }

    mean = compute_mean(nums, n);
    stdev = sqrt(compute_variance(nums, n, mean, 1));

    for (i = 0; i < count; i++) {
        double d;
        if (sc_val_to_num(&vals[i], &d) == SC_OK && stdev > 0.0)
            zout[i] = (d - mean) / stdev;
        else
            zout[i] = 0.0;
    }

    free(nums);
    return SC_OK;
}

int sci_stats_moving_average(const ScVal *vals, size_t count, int window,
                             double *out)
{
    size_t i, j, start;
    double sum;
    int w;

    if (!vals || !out || window <= 0)
        return SC_ERR_INTERNAL;

    w = window;
    for (i = 0; i < count; i++) {
        if (i + 1 < (size_t)w)
            w = (int)(i + 1);
        start = i + 1 - (size_t)w;
        sum = 0.0;
        for (j = start; j <= i; j++) {
            double d;
            if (sc_val_to_num(&vals[j], &d) == SC_OK)
                sum += d;
        }
        out[i] = sum / (double)w;
    }
    return SC_OK;
}

double sci_stats_geometric_mean(ScVal *vals, size_t count)
{
    double *nums = NULL;
    size_t n = 0, i;
    double log_sum = 0.0;

    if (!vals)
        return 0.0;
    if (extract_numbers(vals, count, &nums, &n) != SC_OK || n == 0) {
        free(nums);
        return 0.0;
    }

    for (i = 0; i < n; i++) {
        if (nums[i] <= 0.0) {
            free(nums);
            return 0.0;
        }
        log_sum += log(nums[i]);
    }

    {
        double gm = exp(log_sum / (double)n);
        free(nums);
        return gm;
    }
}

double sci_stats_harmonic_mean(ScVal *vals, size_t count)
{
    double *nums = NULL;
    size_t n = 0, i;
    double recip_sum = 0.0;

    if (!vals)
        return 0.0;
    if (extract_numbers(vals, count, &nums, &n) != SC_OK || n == 0) {
        free(nums);
        return 0.0;
    }

    for (i = 0; i < n; i++) {
        if (nums[i] == 0.0) {
            free(nums);
            return 0.0;
        }
        recip_sum += 1.0 / nums[i];
    }

    {
        double hm = (double)n / recip_sum;
        free(nums);
        return hm;
    }
}
