/*
 * pivot.c — pivot table summarization for sheetcalc
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

enum {
    SC_PIVOT_SUM   = 0,
    SC_PIVOT_COUNT = 1,
    SC_PIVOT_AVG   = 2,
    SC_PIVOT_MIN   = 3,
    SC_PIVOT_MAX   = 4
};

typedef struct {
    char *row_label;
    char *col_label;
    CsPivotBucket *vals;
    int nvals;
} CsPivotEntry;

struct CsPivot {
    ScSheet *sheet;
    int r0, c0, r1, c1;
    int row_field;
    int col_field;
    CsPivotSpec *specs;
    int nspecs;
    CsPivotEntry *entries;
    size_t entry_count, entry_cap;
    ScVal *result;
    size_t result_rows, result_cols;
    int finalized;
};

static char *dup_label_from_val(const ScVal *v)
{
    ScBuf sb;
    char *s;

    if (!v)
        return strdup("");
    if (v->type == SC_VAL_STR && v->as.str.s) {
        s = (char *)malloc(v->as.str.len + 1);
        if (!s)
            return NULL;
        if (v->as.str.len)
            memcpy(s, v->as.str.s, v->as.str.len);
        s[v->as.str.len] = '\0';
        return s;
    }
    sci_buf_init(&sb);
    if (sc_val_to_str(v, &sb) != SC_OK) {
        sci_buf_free(&sb);
        return strdup("");
    }
    s = sci_buf_cstr(&sb);
    if (!s) {
        sci_buf_free(&sb);
        return NULL;
    }
    s = strdup(s);
    sci_buf_free(&sb);
    return s;
}

static int val_at(ScSheet *sh, int row, int col, ScVal *out)
{
    ScCell *cl;

    sc_val_nil(out);
    if (sci_cell_at(sh, row, col, &cl) != SC_OK)
        return SC_ERR_RANGE;
    return sc_val_copy(out, &cl->value);
}

static void bucket_init(CsPivotBucket *b)
{
    if (!b)
        return;
    free(b->label);
    b->label = NULL;
    b->value = 0.0;
    b->count = 0;
    b->sum = 0.0;
    b->min = DBL_MAX;
    b->max = -DBL_MAX;
}

static void bucket_free(CsPivotBucket *b)
{
    if (!b)
        return;
    free(b->label);
    b->label = NULL;
}

static void bucket_accumulate(CsPivotBucket *b, const ScVal *v, int agg_type)
{
    double n;
    int is_num;

    if (!b)
        return;

    b->count++;
    is_num = (sc_val_to_num(v, &n) == SC_OK);
    if (is_num) {
        b->sum += n;
        if (b->count == 1 || n < b->min)
            b->min = n;
        if (b->count == 1 || n > b->max)
            b->max = n;
    } else if (b->count == 1) {
        b->min = 0.0;
        b->max = 0.0;
    }
    (void)agg_type;
}

static double bucket_finalize_value(const CsPivotBucket *b, int agg_type)
{
    if (!b || b->count == 0)
        return 0.0;

    switch (agg_type) {
    case SC_PIVOT_SUM:
        return b->sum;
    case SC_PIVOT_COUNT:
        return (double)b->count;
    case SC_PIVOT_AVG:
        if (b->count == 0)
            return 0.0;
        return b->sum / (double)b->count;
    case SC_PIVOT_MIN:
        return b->min == DBL_MAX ? 0.0 : b->min;
    case SC_PIVOT_MAX:
        return b->max == -DBL_MAX ? 0.0 : b->max;
    default:
        return 0.0;
    }
}

static int labels_equal(const char *a, const char *b)
{
    if (!a && !b)
        return 1;
    if (!a || !b)
        return 0;
    return strcmp(a, b) == 0;
}

static int cmp_cstr(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    if (!*sa && !*sb)
        return 0;
    if (!*sa)
        return -1;
    if (!*sb)
        return 1;
    return strcmp(*sa, *sb);
}

static CsPivotEntry *find_entry(CsPivot *pv, const char *row_l, const char *col_l)
{
    size_t i;

    for (i = 0; i < pv->entry_count; i++) {
        if (labels_equal(pv->entries[i].row_label, row_l) &&
            labels_equal(pv->entries[i].col_label, col_l))
            return &pv->entries[i];
    }
    return NULL;
}

static int ensure_entry(CsPivot *pv, const char *row_l, const char *col_l,
                          CsPivotEntry **out)
{
    CsPivotEntry *e, *ne;
    CsPivotBucket *vb;
    size_t nc;
    int i;

    e = find_entry(pv, row_l, col_l);
    if (e) {
        *out = e;
        return SC_OK;
    }

    if (pv->entry_count >= pv->entry_cap) {
        nc = pv->entry_cap ? pv->entry_cap * 2 : 16;
        ne = (CsPivotEntry *)realloc(pv->entries, nc * sizeof(CsPivotEntry));
        if (!ne)
            return SC_ERR_NOMEM;
        pv->entries = ne;
        pv->entry_cap = nc;
    }

    e = &pv->entries[pv->entry_count];
    memset(e, 0, sizeof(*e));
    e->row_label = row_l ? strdup(row_l) : strdup("");
    e->col_label = col_l ? strdup(col_l) : strdup("");
    if (!e->row_label || !e->col_label)
        return SC_ERR_NOMEM;

    e->nvals = pv->nspecs;
    vb = (CsPivotBucket *)calloc((size_t)e->nvals, sizeof(CsPivotBucket));
    if (!vb)
        return SC_ERR_NOMEM;
    e->vals = vb;
    for (i = 0; i < e->nvals; i++)
        bucket_init(&e->vals[i]);

    pv->entry_count++;
    *out = e;
    return SC_OK;
}

static void free_entry(CsPivotEntry *e)
{
    int i;

    if (!e)
        return;
    free(e->row_label);
    free(e->col_label);
    e->row_label = e->col_label = NULL;
    if (e->vals) {
        for (i = 0; i < e->nvals; i++)
            bucket_free(&e->vals[i]);
        free(e->vals);
        e->vals = NULL;
    }
    e->nvals = 0;
}

static void clamp_range(CsPivot *pv)
{
    ScSheet *sh = pv->sheet;

    if (pv->r0 < 0)
        pv->r0 = 0;
    if (pv->c0 < 0)
        pv->c0 = 0;
    if (pv->r1 >= sh->nrows)
        pv->r1 = sh->nrows - 1;
    if (pv->c1 >= sh->ncols)
        pv->c1 = sh->ncols - 1;
    if (pv->r0 > pv->r1 || pv->c0 > pv->c1) {
        pv->r1 = pv->r0 - 1;
        pv->c1 = pv->c0 - 1;
    }
}

CsPivot *sci_pivot_create(ScSheet *sh, CsPivotSpec *specs, int nspecs,
                          int r0, int c0, int r1, int c1,
                          int row_field, int col_field)
{
    CsPivot *pv;
    CsPivotSpec *sp_copy;
    int i;

    if (!sh || !specs || nspecs <= 0)
        return NULL;

    pv = (CsPivot *)calloc(1, sizeof(CsPivot));
    if (!pv)
        return NULL;

    sp_copy = (CsPivotSpec *)malloc((size_t)nspecs * sizeof(CsPivotSpec));
    if (!sp_copy) {
        free(pv);
        return NULL;
    }
    memcpy(sp_copy, specs, (size_t)nspecs * sizeof(CsPivotSpec));

    pv->sheet = sh;
    pv->r0 = r0;
    pv->c0 = c0;
    pv->r1 = r1;
    pv->c1 = c1;
    pv->row_field = row_field;
    pv->col_field = col_field;
    pv->specs = sp_copy;
    pv->nspecs = nspecs;
    clamp_range(pv);
    for (i = 0; i < nspecs; i++) {
        if (pv->specs[i].agg_type < SC_PIVOT_SUM ||
            pv->specs[i].agg_type > SC_PIVOT_MAX)
            pv->specs[i].agg_type = SC_PIVOT_SUM;
    }
    return pv;
}

void sci_pivot_free(CsPivot *pv)
{
    size_t i;

    if (!pv)
        return;

    if (pv->entries) {
        for (i = 0; i < pv->entry_count; i++)
            free_entry(&pv->entries[i]);
        free(pv->entries);
    }
    free(pv->specs);

    if (pv->result) {
        for (i = 0; i < pv->result_rows * pv->result_cols; i++)
            sc_val_free(&pv->result[i]);
        free(pv->result);
    }

    free(pv);
}

int sci_pivot_add_row(CsPivot *pv, int row)
{
    ScVal row_v, col_v, val_v;
    char *row_l, *col_l;
    CsPivotEntry *ent;
    int c, si, rc;

    if (!pv || pv->finalized)
        return SC_ERR_INTERNAL;
    if (row < pv->r0 || row > pv->r1)
        return SC_ERR_RANGE;

    sc_val_nil(&row_v);
    sc_val_nil(&col_v);

    if (pv->row_field >= pv->c0 && pv->row_field <= pv->c1) {
        rc = val_at(pv->sheet, row, pv->row_field, &row_v);
        if (rc != SC_OK)
            row_l = strdup("");
        else
            row_l = dup_label_from_val(&row_v);
    } else {
        row_l = strdup("");
    }

    if (pv->col_field >= pv->c0 && pv->col_field <= pv->c1) {
        rc = val_at(pv->sheet, row, pv->col_field, &col_v);
        if (rc != SC_OK)
            col_l = strdup("");
        else
            col_l = dup_label_from_val(&col_v);
    } else {
        col_l = strdup("");
    }

    if (!row_l || !col_l) {
        free(row_l);
        free(col_l);
        sc_val_free(&row_v);
        sc_val_free(&col_v);
        return SC_ERR_NOMEM;
    }

    rc = ensure_entry(pv, row_l, col_l, &ent);
    free(row_l);
    free(col_l);
    sc_val_free(&row_v);
    sc_val_free(&col_v);
    if (rc != SC_OK)
        return rc;

    for (si = 0; si < pv->nspecs; si++) {
        int vcol = pv->specs[si].value_col;
        if (vcol < pv->c0 || vcol > pv->c1)
            continue;
        sc_val_nil(&val_v);
        if (val_at(pv->sheet, row, vcol, &val_v) != SC_OK) {
            sc_val_free(&val_v);
            continue;
        }
        bucket_accumulate(&ent->vals[si], &val_v, pv->specs[si].agg_type);
        sc_val_free(&val_v);
    }

    for (c = pv->c0; c <= pv->c1; c++) {
        if (c == pv->row_field || c == pv->col_field)
            continue;
        for (si = 0; si < pv->nspecs; si++) {
            if (pv->specs[si].group_col == c) {
                sc_val_nil(&val_v);
                if (val_at(pv->sheet, row, c, &val_v) == SC_OK)
                    bucket_accumulate(&ent->vals[si], &val_v,
                                        pv->specs[si].agg_type);
                sc_val_free(&val_v);
            }
        }
    }

    return SC_OK;
}

static int collect_unique_labels(CsPivot *pv, char ***rows, size_t *nrows,
                                 char ***cols, size_t *ncols)
{
    char **rl = NULL, **cl = NULL;
    size_t nr = 0, nc = 0, nr_cap = 0, nc_cap = 0;
    size_t i;
    int found;

    for (i = 0; i < pv->entry_count; i++) {
        const char *r = pv->entries[i].row_label ? pv->entries[i].row_label : "";
        const char *c = pv->entries[i].col_label ? pv->entries[i].col_label : "";

        found = 0;
        for (size_t j = 0; j < nr; j++) {
            if (labels_equal(rl[j], r)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            if (nr >= nr_cap) {
                nr_cap = nr_cap ? nr_cap * 2 : 8;
                rl = (char **)realloc(rl, nr_cap * sizeof(char *));
                if (!rl)
                    goto fail;
            }
            rl[nr++] = strdup(r);
            if (!rl[nr - 1])
                goto fail;
        }

        found = 0;
        for (size_t j = 0; j < nc; j++) {
            if (labels_equal(cl[j], c)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            if (nc >= nc_cap) {
                nc_cap = nc_cap ? nc_cap * 2 : 8;
                cl = (char **)realloc(cl, nc_cap * sizeof(char *));
                if (!cl)
                    goto fail;
            }
            cl[nc++] = strdup(c);
            if (!cl[nc - 1])
                goto fail;
        }
    }

    if (nr > 1)
        qsort(rl, nr, sizeof(char *), cmp_cstr);
    if (nc > 1)
        qsort(cl, nc, sizeof(char *), cmp_cstr);

    *rows = rl;
    *nrows = nr;
    *cols = cl;
    *ncols = nc;
    return SC_OK;

fail:
    for (i = 0; i < nr; i++)
        free(rl[i]);
    for (i = 0; i < nc; i++)
        free(cl[i]);
    free(rl);
    free(cl);
    return SC_ERR_NOMEM;
}

static void free_label_list(char **labels, size_t n)
{
    size_t i;
    if (!labels)
        return;
    for (i = 0; i < n; i++)
        free(labels[i]);
    free(labels);
}

int sci_pivot_finalize(CsPivot *pv)
{
    char **row_labels = NULL, **col_labels = NULL;
    size_t n_row_labels = 0, n_col_labels = 0;
    size_t out_rows, out_cols, idx, ri, ci, vi;
    ScVal *out;
    int rc, r;

    if (!pv)
        return SC_ERR_INTERNAL;
    if (pv->finalized)
        return SC_OK;

    for (r = pv->r0; r <= pv->r1; r++) {
        rc = sci_pivot_add_row(pv, r);
        if (rc != SC_OK && rc != SC_ERR_RANGE)
            return rc;
    }

    rc = collect_unique_labels(pv, &row_labels, &n_row_labels,
                               &col_labels, &n_col_labels);
    if (rc != SC_OK)
        return rc;

    if (n_row_labels == 0)
        n_row_labels = 1;
    if (n_col_labels == 0)
        n_col_labels = 1;

    out_rows = n_row_labels + 1;
    out_cols = 1 + n_col_labels * (size_t)pv->nspecs;
    out = (ScVal *)calloc(out_rows * out_cols, sizeof(ScVal));
    if (!out) {
        free_label_list(row_labels, n_row_labels);
        free_label_list(col_labels, n_col_labels);
        return SC_ERR_NOMEM;
    }

    sc_val_str(&out[0], "");
    for (ci = 0; ci < n_col_labels; ci++) {
        for (vi = 0; vi < (size_t)pv->nspecs; vi++) {
            idx = 1 + ci * (size_t)pv->nspecs + vi;
            sc_val_str(&out[idx], col_labels ? col_labels[ci] : "");
        }
    }

    for (ri = 0; ri < n_row_labels; ri++) {
        idx = (ri + 1) * out_cols;
        sc_val_str(&out[idx], row_labels ? row_labels[ri] : "");

        for (ci = 0; ci < n_col_labels; ci++) {
            const char *rl = row_labels ? row_labels[ri] : "";
            const char *cl = col_labels ? col_labels[ci] : "";
            CsPivotEntry *ent = find_entry(pv, rl, cl);

            for (vi = 0; vi < (size_t)pv->nspecs; vi++) {
                double val = 0.0;
                size_t col_idx = 1 + ci * (size_t)pv->nspecs + vi;

                if (ent)
                    val = bucket_finalize_value(&ent->vals[vi],
                                                pv->specs[vi].agg_type);
                sc_val_num(&out[idx + col_idx], val);
            }
        }
    }

    if (pv->result) {
        for (idx = 0; idx < pv->result_rows * pv->result_cols; idx++)
            sc_val_free(&pv->result[idx]);
        free(pv->result);
    }

    pv->result = out;
    pv->result_rows = out_rows;
    pv->result_cols = out_cols;
    pv->finalized = 1;

    free_label_list(row_labels, n_row_labels);
    free_label_list(col_labels, n_col_labels);
    return SC_OK;
}

int sci_pivot_get_result(CsPivot *pv, ScVal **out, size_t *nrows, size_t *ncols)
{
    ScVal *copy;
    size_t i, n;

    if (!pv || !out || !nrows || !ncols)
        return SC_ERR_INTERNAL;
    if (!pv->finalized)
        return SC_ERR_INTERNAL;

    n = pv->result_rows * pv->result_cols;
    copy = (ScVal *)calloc(n, sizeof(ScVal));
    if (!copy)
        return SC_ERR_NOMEM;

    for (i = 0; i < n; i++) {
        sc_val_nil(&copy[i]);
        if (sc_val_copy(&copy[i], &pv->result[i]) != SC_OK) {
            while (i > 0)
                sc_val_free(&copy[--i]);
            free(copy);
            return SC_ERR_NOMEM;
        }
    }

    *out = copy;
    *nrows = pv->result_rows;
    *ncols = pv->result_cols;
    return SC_OK;
}

int sci_pivot_bucket_sort(CsPivotBucket *buckets, size_t count, int descending)
{
    size_t i, j;
    CsPivotBucket tmp;

    if (!buckets || count < 2)
        return SC_OK;

    for (i = 0; i + 1 < count; i++) {
        for (j = i + 1; j < count; j++) {
            int swap = descending ? (buckets[i].value < buckets[j].value)
                                  : (buckets[i].value > buckets[j].value);
            if (swap) {
                tmp = buckets[i];
                buckets[i] = buckets[j];
                buckets[j] = tmp;
            }
        }
    }
    return SC_OK;
}

int sci_pivot_match_label(const CsPivotBucket *b, const char *label)
{
    if (!b || !label)
        return 0;
    if (!b->label)
        return label[0] == '\0';
    return strcmp(b->label, label) == 0;
}

void sci_pivot_bucket_set_label(CsPivotBucket *b, const char *label)
{
    if (!b)
        return;
    free(b->label);
    b->label = label ? strdup(label) : NULL;
}

double sci_pivot_agg_value(const CsPivotBucket *b, int agg_type)
{
    return bucket_finalize_value(b, agg_type);
}
