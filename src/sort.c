/*
 * sort.c — multi-key range sorting for sheetcalc
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

typedef struct {
    ScVal *row;
    int ncols;
    int orig_idx;
} SortRow;

typedef struct {
    CsSortKey *keys;
    int nkeys;
} SortCtx;

static int cmp_num_key(double a, double b)
{
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static int compare_vals_key(const ScVal *a, const ScVal *b, const CsSortKey *key)
{
    double na, nb;
    int c;

    if (!a || !b)
        return 0;

    if (key->numeric) {
        if (sc_val_to_num(a, &na) != SC_OK)
            na = key->descending ? -DBL_MAX : DBL_MAX;
        if (sc_val_to_num(b, &nb) != SC_OK)
            nb = key->descending ? -DBL_MAX : DBL_MAX;
        c = cmp_num_key(na, nb);
    } else {
        c = sc_val_cmp(a, b);
        if (c == 0 && a->type != b->type) {
            if (a->type == SC_VAL_EMPTY || a->type == SC_VAL_NIL)
                c = -1;
            else if (b->type == SC_VAL_EMPTY || b->type == SC_VAL_NIL)
                c = 1;
        }
    }

    if (key->descending)
        c = -c;
    return c;
}

static int compare_rows(const SortRow *a, const SortRow *b, const SortCtx *ctx)
{
    int i, c;

    for (i = 0; i < ctx->nkeys; i++) {
        int col = ctx->keys[i].col;
        const ScVal *va, *vb;

        if (col < 0 || col >= a->ncols || col >= b->ncols)
            continue;
        va = &a->row[col];
        vb = &b->row[col];
        c = compare_vals_key(va, vb, &ctx->keys[i]);
        if (c != 0)
            return c;
    }

    if (a->orig_idx < b->orig_idx)
        return -1;
    if (a->orig_idx > b->orig_idx)
        return 1;
    return 0;
}

static void merge_rows(SortRow *dst, SortRow *src, size_t n, SortRow *tmp,
                       const SortCtx *ctx)
{
    size_t i = 0, j = n, k = 0;

    while (i < n && j < 2 * n) {
        if (compare_rows(&src[i], &src[j], ctx) <= 0)
            tmp[k++] = src[i++];
        else
            tmp[k++] = src[j++];
    }
    while (i < n)
        tmp[k++] = src[i++];
    while (j < 2 * n)
        tmp[k++] = src[j++];
    memcpy(dst, tmp, 2 * n * sizeof(SortRow));
}

static void merge_sort_rows(SortRow *rows, SortRow *tmp, size_t n,
                            const SortCtx *ctx)
{
    size_t w;

    if (n < 2)
        return;

    for (w = 1; w < n; w *= 2) {
        size_t i;
        for (i = 0; i + 2 * w <= n; i += 2 * w)
            merge_rows(rows + i, rows + i, w, tmp + i, ctx);
        if (i + w < n)
            merge_rows(rows + i, rows + i, n - i - w, tmp + i, ctx);
    }
}

static void free_sort_rows(SortRow *rows, size_t count, int ncols)
{
    size_t i;
    int c;

    if (!rows)
        return;
    for (i = 0; i < count; i++) {
        if (rows[i].row) {
            for (c = 0; c < ncols; c++)
                sc_val_free(&rows[i].row[c]);
            free(rows[i].row);
        }
    }
    free(rows);
}

static int extract_range_rows(ScSheet *sh, int r0, int c0, int r1, int c1,
                              SortRow **out_rows, size_t *out_count)
{
    int ncols = c1 - c0 + 1;
    int nrows = r1 - r0 + 1;
    SortRow *rows;
    int r, c;

    if (ncols <= 0 || nrows <= 0) {
        *out_rows = NULL;
        *out_count = 0;
        return SC_OK;
    }

    rows = (SortRow *)calloc((size_t)nrows, sizeof(SortRow));
    if (!rows)
        return SC_ERR_NOMEM;

    for (r = 0; r < nrows; r++) {
        rows[r].ncols = ncols;
        rows[r].orig_idx = r;
        rows[r].row = (ScVal *)calloc((size_t)ncols, sizeof(ScVal));
        if (!rows[r].row) {
            free_sort_rows(rows, (size_t)r, ncols);
            return SC_ERR_NOMEM;
        }
        for (c = 0; c < ncols; c++) {
            ScCell *cl;
            sc_val_nil(&rows[r].row[c]);
            if (sci_cell_at(sh, r0 + r, c0 + c, &cl) == SC_OK) {
                if (sc_val_copy(&rows[r].row[c], &cl->value) != SC_OK) {
                    free_sort_rows(rows, (size_t)nrows, ncols);
                    return SC_ERR_NOMEM;
                }
            }
        }
    }

    *out_rows = rows;
    *out_count = (size_t)nrows;
    return SC_OK;
}

static int write_back_rows(ScSheet *sh, int r0, int c0, int c1,
                           SortRow *rows, size_t count)
{
    int ncols = c1 - c0 + 1;
    size_t i;
    int c;

    (void)ncols;

    for (i = 0; i < count; i++) {
        for (c = 0; c < ncols; c++) {
            ScCell *cl;
            if (sci_cell_at(sh, r0 + (int)i, c0 + c, &cl) != SC_OK)
                continue;
            sc_val_free(&cl->value);
            sc_val_copy(&cl->value, &rows[i].row[c]);
        }
    }
    return SC_OK;
}

int sci_sort_vals(ScVal *vals, size_t count, CsSortKey *keys, int nkeys,
                  int ncols)
{
    SortRow *rows, *tmp;
    SortCtx ctx;
    size_t nrows, i;
    int c;

    if (!vals || !keys || nkeys <= 0 || ncols <= 0 || count == 0)
        return SC_ERR_INTERNAL;

    if (count % (size_t)ncols != 0)
        return SC_ERR_RANGE;

    nrows = count / (size_t)ncols;
    rows = (SortRow *)calloc(nrows, sizeof(SortRow));
    tmp = (SortRow *)calloc(nrows, sizeof(SortRow));
    if (!rows || !tmp) {
        free(rows);
        free(tmp);
        return SC_ERR_NOMEM;
    }

    for (i = 0; i < nrows; i++) {
        rows[i].ncols = ncols;
        rows[i].orig_idx = (int)i;
        rows[i].row = vals + i * (size_t)ncols;
    }

    ctx.keys = keys;
    ctx.nkeys = nkeys;
    merge_sort_rows(rows, tmp, nrows, &ctx);

    {
        ScVal *sorted = (ScVal *)calloc(count, sizeof(ScVal));
        if (!sorted) {
            free(rows);
            free(tmp);
            return SC_ERR_NOMEM;
        }
        for (i = 0; i < nrows; i++) {
            for (c = 0; c < ncols; c++) {
                size_t dst = i * (size_t)ncols + (size_t)c;
                sc_val_nil(&sorted[dst]);
                if (sc_val_copy(&sorted[dst], &rows[i].row[c]) != SC_OK) {
                    for (size_t k = 0; k <= dst; k++)
                        sc_val_free(&sorted[k]);
                    free(sorted);
                    free(rows);
                    free(tmp);
                    return SC_ERR_NOMEM;
                }
            }
        }
        for (i = 0; i < count; i++)
            sc_val_free(&vals[i]);
        memcpy(vals, sorted, count * sizeof(ScVal));
        free(sorted);
    }

    free(rows);
    free(tmp);
    return SC_OK;
}

int sci_sort_range(ScSheet *sh, int r0, int c0, int r1, int c1,
                   CsSortKey *keys, int nkeys)
{
    SortRow *rows, *tmp;
    SortCtx ctx;
    size_t count;
    int rc, ncols;

    if (!sh || !keys || nkeys <= 0)
        return SC_ERR_INTERNAL;

    if (r0 < 0 || c0 < 0 || r1 >= sh->nrows || c1 >= sh->ncols || r0 > r1 || c0 > c1)
        return SC_ERR_RANGE;

    ncols = c1 - c0 + 1;
    rc = extract_range_rows(sh, r0, c0, r1, c1, &rows, &count);
    if (rc != SC_OK)
        return rc;
    if (count == 0) {
        free(rows);
        return SC_OK;
    }

    tmp = (SortRow *)calloc(count, sizeof(SortRow));
    if (!tmp) {
        free_sort_rows(rows, count, ncols);
        return SC_ERR_NOMEM;
    }

    ctx.keys = keys;
    ctx.nkeys = nkeys;
    merge_sort_rows(rows, tmp, count, &ctx);

    rc = write_back_rows(sh, r0, c0, c1, rows, count);
    free_sort_rows(rows, count, ncols);
    free(tmp);
    return rc;
}

typedef struct {
    int orig_row;
    int rank;
} RankEntry;

static int cmp_rank_rows(const void *a, const void *b, void *arg)
{
    const RankEntry *ra = (const RankEntry *)a;
    const RankEntry *rb = (const RankEntry *)b;
    SortRow *rows = (SortRow *)arg;
    SortCtx ctx;
    CsSortKey key;
    int c;

    key.col = 0;
    key.descending = 0;
    key.numeric = 1;
    ctx.keys = &key;
    ctx.nkeys = 1;

    c = compare_rows(&rows[ra->orig_row], &rows[rb->orig_row], &ctx);
    if (c != 0)
        return c;
    return ra->orig_row - rb->orig_row;
}

static void insertion_sort_rank(RankEntry *entries, size_t n, SortRow *rows)
{
    size_t i, j;
    RankEntry tmp;

    for (i = 1; i < n; i++) {
        tmp = entries[i];
        j = i;
        while (j > 0 && cmp_rank_rows(&tmp, &entries[j - 1], rows) < 0) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = tmp;
    }
}

int csi_rank_range(ScSheet *sh, int r0, int c0, int r1, int c1,
                   int key_col, int numeric, ScVal *ranks_out)
{
    SortRow *rows;
    RankEntry *rent;
    size_t count, i;
    int ncols, rc;
    int cur_rank;
    double prev_num;
    int has_prev;

    if (!sh || !ranks_out)
        return SC_ERR_INTERNAL;
    if (r0 < 0 || c0 < 0 || r1 >= sh->nrows || c1 >= sh->ncols)
        return SC_ERR_RANGE;
    if (key_col < c0 || key_col > c1)
        return SC_ERR_RANGE;

    ncols = c1 - c0 + 1;
    rc = extract_range_rows(sh, r0, c0, r1, c1, &rows, &count);
    if (rc != SC_OK)
        return rc;

    rent = (RankEntry *)calloc(count, sizeof(RankEntry));
    if (!rent) {
        free_sort_rows(rows, count, ncols);
        return SC_ERR_NOMEM;
    }

    for (i = 0; i < count; i++)
        rent[i].orig_row = (int)i;

    {
        CsSortKey sk;
        sk.col = key_col - c0;
        sk.descending = 0;
        sk.numeric = numeric;
        SortCtx ctx = { &sk, 1 };
        SortRow *tmp = (SortRow *)calloc(count, sizeof(SortRow));
        if (tmp) {
            merge_sort_rows(rows, tmp, count, &ctx);
            free(tmp);
        }
    }

    insertion_sort_rank(rent, count, rows);

    cur_rank = 1;
    has_prev = 0;
    prev_num = 0.0;

    for (i = 0; i < count; i++) {
        int ri = rent[i].orig_row;
        double n = 0.0;
        int tie = 0;

        if (numeric && sc_val_to_num(&rows[ri].row[key_col - c0], &n) == SC_OK) {
            if (has_prev && n == prev_num)
                tie = 1;
            prev_num = n;
            has_prev = 1;
        }

        if (!tie && i > 0)
            cur_rank = (int)i + 1;

        sc_val_num(&ranks_out[ri], (double)cur_rank);
    }

    free(rent);
    free_sort_rows(rows, count, ncols);
    return SC_OK;
}

static int vals_equal_row(const ScVal *a, const ScVal *b, int ncols)
{
    int c;
    for (c = 0; c < ncols; c++) {
        if (sc_val_cmp(&a[c], &b[c]) != 0)
            return 0;
    }
    return 1;
}

int csi_unique_range(ScSheet *sh, int r0, int c0, int r1, int c1,
                     int *out_r1)
{
    SortRow *rows;
    size_t count, w, r;
    int ncols, rc;
    CsSortKey key;

    if (!sh)
        return SC_ERR_INTERNAL;
    if (r0 < 0 || c0 < 0 || r1 >= sh->nrows || c1 >= sh->ncols || r0 > r1)
        return SC_ERR_RANGE;

    ncols = c1 - c0 + 1;
    rc = extract_range_rows(sh, r0, c0, r1, c1, &rows, &count);
    if (rc != SC_OK)
        return rc;
    if (count == 0) {
        if (out_r1)
            *out_r1 = r0 - 1;
        free(rows);
        return SC_OK;
    }

    key.col = 0;
    key.descending = 0;
    key.numeric = 0;
    {
        SortCtx ctx = { &key, 1 };
        SortRow *tmp = (SortRow *)calloc(count, sizeof(SortRow));
        if (tmp) {
            for (int k = 1; k < ncols; k++) {
                CsSortKey ek;
                ek.col = k;
                ek.descending = 0;
                ek.numeric = 0;
                ctx.keys = &ek;
                ctx.nkeys = 1;
            }
            ctx.keys = &key;
            ctx.nkeys = 1;
            merge_sort_rows(rows, tmp, count, &ctx);
            free(tmp);
        }
    }

    w = 1;
    for (r = 1; r < count; r++) {
        if (!vals_equal_row(rows[r].row, rows[w - 1].row, ncols)) {
            if (w != r) {
                int c;
                for (c = 0; c < ncols; c++) {
                    sc_val_free(&rows[w].row[c]);
                    sc_val_copy(&rows[w].row[c], &rows[r].row[c]);
                }
            }
            w++;
        }
    }

    rc = write_back_rows(sh, r0, c0, c1, rows, w);

    for (r = w; r < count; r++) {
        int c;
        for (c = 0; c < ncols; c++)
            sc_val_free(&rows[r].row[c]);
    }

    if (out_r1)
        *out_r1 = r0 + (int)w - 1;

    free_sort_rows(rows, count, ncols);
    return rc;
}

int sci_sort_compare_key(const ScVal *a, const ScVal *b, const CsSortKey *key)
{
    return compare_vals_key(a, b, key);
}

int sci_sort_stable(ScVal *vals, size_t count, CsSortKey *keys, int nkeys,
                    int ncols)
{
    return sci_sort_vals(vals, count, keys, nkeys, ncols);
}

static void merge_sort_vals_direct(ScVal *base, ScVal *tmp, size_t count,
                                   int ncols, const SortCtx *ctx)
{
    size_t w, i;

    if (count < 2)
        return;

    for (w = 1; w < count; w *= 2) {
        for (i = 0; i + 2 * w <= count; i += 2 * w) {
            size_t k = 0, left = i, mid = i + w, right = i + 2 * w;
            SortRow la, lb;

            la.ncols = ncols;
            lb.ncols = ncols;
            while (left < i + w && mid < right) {
                la.row = base + left * (size_t)ncols;
                lb.row = base + mid * (size_t)ncols;
                la.orig_idx = (int)left;
                lb.orig_idx = (int)mid;
                if (compare_rows(&la, &lb, ctx) <= 0) {
                    memcpy(tmp + k * (size_t)ncols, base + left * (size_t)ncols,
                           (size_t)ncols * sizeof(ScVal));
                    left++;
                } else {
                    memcpy(tmp + k * (size_t)ncols, base + mid * (size_t)ncols,
                           (size_t)ncols * sizeof(ScVal));
                    mid++;
                }
                k++;
            }
            while (left < i + w) {
                memcpy(tmp + k * (size_t)ncols, base + left * (size_t)ncols,
                       (size_t)ncols * sizeof(ScVal));
                left++;
                k++;
            }
            while (mid < right) {
                memcpy(tmp + k * (size_t)ncols, base + mid * (size_t)ncols,
                       (size_t)ncols * sizeof(ScVal));
                mid++;
                k++;
            }
            memcpy(base + i * (size_t)ncols, tmp,
                   k * (size_t)ncols * sizeof(ScVal));
        }
    }
}

int sci_sort_vals_inplace(ScVal *vals, size_t row_count, CsSortKey *keys,
                          int nkeys, int ncols)
{
    ScVal *tmp;
    SortCtx ctx;

    if (!vals || !keys || nkeys <= 0 || ncols <= 0 || row_count == 0)
        return SC_ERR_INTERNAL;

    tmp = (ScVal *)malloc(row_count * (size_t)ncols * sizeof(ScVal));
    if (!tmp)
        return SC_ERR_NOMEM;

    ctx.keys = keys;
    ctx.nkeys = nkeys;
    merge_sort_vals_direct(vals, tmp, row_count, ncols, &ctx);
    free(tmp);
    return SC_OK;
}
