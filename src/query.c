/*
 * query.c — cell query / filter engine for sheetcalc
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

#include "../include/sc_internal.h"

#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int sheet;
    int r0, c0, r1, c1;  /* range to query */
    int type_filter;      /* SC_VAL_xxx or -1 for any */
    double num_min, num_max;  /* for numeric range filter */
    const char *str_match;    /* substring match for strings */
    int has_formula;      /* -1 = any, 0 = no formula, 1 = has formula */
} CsQuery;

typedef struct {
    int sheet, row, col;
    ScVal value;
} CsQueryResult;

typedef struct {
    CsQueryResult *results;
    size_t count, cap;
} CsQueryResults;

enum { SC_AGG_SUM = 0, SC_AGG_AVG, SC_AGG_MIN, SC_AGG_MAX, SC_AGG_COUNT };

static void clamp_query_range(const ScWorkbook *wb, CsQuery *q, int sh_idx)
{
    ScSheet *sh = &wb->sheets[sh_idx];
    if (q->r0 < 0)
        q->r0 = 0;
    if (q->c0 < 0)
        q->c0 = 0;
    if (q->r1 < 0 || q->r1 >= sh->nrows)
        q->r1 = sh->nrows - 1;
    if (q->c1 < 0 || q->c1 >= sh->ncols)
        q->c1 = sh->ncols - 1;
    if (q->r0 > q->r1 || q->c0 > q->c1) {
        q->r1 = q->r0 - 1;
        q->c1 = q->c0 - 1;
    }
}

static int str_contains(const ScVal *v, const char *needle)
{
    const char *hay;
    size_t hlen, nlen;

    if (!needle || !needle[0])
        return 1;
    if (v->type != SC_VAL_STR || !v->as.str.s)
        return 0;
    hay = v->as.str.s;
    hlen = v->as.str.len;
    nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen)
        return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

static int cell_matches(const ScCell *c, const CsQuery *q)
{
    double n;

    if (!c || !q)
        return 0;

    if (q->type_filter >= 0) {
        int ty = c->value.type;
        if (ty == SC_VAL_NIL)
            ty = SC_VAL_EMPTY;
        if (ty != q->type_filter)
            return 0;
    }

    if (q->has_formula == 0 && c->formula)
        return 0;
    if (q->has_formula == 1 && !c->formula)
        return 0;

    if (q->type_filter < 0 || q->type_filter == SC_VAL_NUM ||
        c->value.type == SC_VAL_NUM) {
        if (sc_val_to_num(&c->value, &n) == SC_OK) {
            if (n < q->num_min || n > q->num_max)
                return 0;
        } else if (q->type_filter == SC_VAL_NUM) {
            return 0;
        }
    }

    if (q->str_match && !str_contains(&c->value, q->str_match))
        return 0;

    return 1;
}

static int add_result(CsQueryResults *out, int sheet, int row, int col, const ScVal *v)
{
    CsQueryResult *nr;
    size_t nc;

    if (!out || !v)
        return SC_ERR_INTERNAL;

    if (out->count >= out->cap) {
        nc = out->cap ? out->cap * 2 : 32;
        nr = (CsQueryResult *)realloc(out->results, nc * sizeof(CsQueryResult));
        if (!nr)
            return SC_ERR_NOMEM;
        out->results = nr;
        out->cap = nc;
    }

    out->results[out->count].sheet = sheet;
    out->results[out->count].row = row;
    out->results[out->count].col = col;
    sc_val_nil(&out->results[out->count].value);
    if (sc_val_copy(&out->results[out->count].value, v) != SC_OK)
        return SC_ERR_NOMEM;
    out->count++;
    return SC_OK;
}

typedef int (*CellVisitFn)(ScWorkbook *wb, int sh, int r, int c,
                           ScCell *cl, void *ctx);

static int scan_query(ScWorkbook *wb, const CsQuery *q,
                      CellVisitFn fn, void *ctx, int *stop)
{
    int sh_start, sh_end, si, r, c, rc;
    CsQuery local;

    if (!wb || !q || !fn)
        return SC_ERR_INTERNAL;

    memcpy(&local, q, sizeof(local));

    if (local.sheet >= 0) {
        if (local.sheet >= wb->nsheets)
            return SC_ERR_RANGE;
        sh_start = local.sheet;
        sh_end = local.sheet;
    } else {
        sh_start = 0;
        sh_end = wb->nsheets - 1;
    }

    for (si = sh_start; si <= sh_end; si++) {
        ScSheet *sh = &wb->sheets[si];
        CsQuery rq = local;

        if (!sh->cells || sh->nrows <= 0 || sh->ncols <= 0)
            continue;
        clamp_query_range(wb, &rq, si);

        for (r = rq.r0; r <= rq.r1; r++) {
            for (c = rq.c0; c <= rq.c1; c++) {
                ScCell *cl;
                if (sci_cell_at(sh, r, c, &cl) != SC_OK)
                    continue;
                if (!cell_matches(cl, &rq))
                    continue;
                rc = fn(wb, si, r, c, cl, ctx);
                if (rc != SC_OK)
                    return rc;
                if (stop && *stop)
                    return SC_OK;
            }
        }
    }
    return SC_OK;
}

static int visit_collect(ScWorkbook *wb, int sh, int r, int c,
                         ScCell *cl, void *ctx)
{
    CsQueryResults *out = (CsQueryResults *)ctx;
    (void)wb;
    return add_result(out, sh, r, c, &cl->value);
}

static int scan_collect(ScWorkbook *wb, const CsQuery *q, CsQueryResults *out)
{
    return scan_query(wb, q, visit_collect, out, NULL);
}

static int visit_count(ScWorkbook *wb, int sh, int r, int c,
                       ScCell *cl, void *ctx)
{
    size_t *cnt = (size_t *)ctx;
    (void)wb; (void)sh; (void)r; (void)c; (void)cl;
    (*cnt)++;
    return SC_OK;
}

typedef struct {
    int agg_type;
    double sum;
    double min;
    double max;
    size_t num_count;
    size_t total;
} AggCtx;

static int visit_agg(ScWorkbook *wb, int sh, int r, int c,
                     ScCell *cl, void *ctx)
{
    AggCtx *ac = (AggCtx *)ctx;
    double n;
    (void)wb; (void)sh; (void)r; (void)c;

    ac->total++;
    if (sc_val_to_num(&cl->value, &n) != SC_OK)
        return SC_OK;

    ac->num_count++;
    ac->sum += n;
    if (ac->num_count == 1) {
        ac->min = n;
        ac->max = n;
    } else {
        if (n < ac->min)
            ac->min = n;
        if (n > ac->max)
            ac->max = n;
    }
    return SC_OK;
}

typedef struct {
    int found;
    int sheet, row, col;
} FirstCtx;

typedef struct {
    FirstCtx *fc;
    int *stop;
} FirstWrap;

static int visit_first(ScWorkbook *wb, int sh, int r, int c,
                       ScCell *cl, void *ctx)
{
    FirstWrap *w = (FirstWrap *)ctx;
    (void)wb; (void)cl;
    w->fc->found = 1;
    w->fc->sheet = sh;
    w->fc->row = r;
    w->fc->col = c;
    if (w->stop)
        *w->stop = 1;
    return SC_OK;
}

typedef struct {
    int (*pred)(const ScVal *, void *);
    void *ctx;
    CsQueryResults *out;
} PredCtx;

static int visit_pred(ScWorkbook *wb, int sh, int r, int c,
                      ScCell *cl, void *ctx)
{
    PredCtx *pc = (PredCtx *)ctx;
    (void)wb;
    if (!pc->pred(&cl->value, pc->ctx))
        return SC_OK;
    return add_result(pc->out, sh, r, c, &cl->value);
}

void sc_query_init(CsQuery *q)
{
    if (!q)
        return;
    q->sheet = -1;
    q->r0 = 0;
    q->c0 = 0;
    q->r1 = -1;
    q->c1 = -1;
    q->type_filter = -1;
    q->num_min = -DBL_MAX;
    q->num_max = DBL_MAX;
    q->str_match = NULL;
    q->has_formula = -1;
}

int sc_query_exec(ScWorkbook *wb, const CsQuery *q, CsQueryResults *out)
{
    if (!wb || !q || !out)
        return SC_ERR_INTERNAL;
    out->count = 0;
    return scan_collect(wb, q, out);
}

void sc_query_free_results(CsQueryResults *r)
{
    size_t i;
    if (!r)
        return;
    for (i = 0; i < r->count; i++)
        sc_val_free(&r->results[i].value);
    free(r->results);
    r->results = NULL;
    r->count = 0;
    r->cap = 0;
}

static int cmp_results(const void *a, const void *b)
{
    const CsQueryResult *ra = (const CsQueryResult *)a;
    const CsQueryResult *rb = (const CsQueryResult *)b;
    int c = sc_val_cmp(&ra->value, &rb->value);
    if (c != 0)
        return c;
    if (ra->sheet != rb->sheet)
        return ra->sheet - rb->sheet;
    if (ra->row != rb->row)
        return ra->row - rb->row;
    return ra->col - rb->col;
}

static int cmp_results_desc(const void *a, const void *b)
{
    return -cmp_results(a, b);
}

void sc_query_sort(CsQueryResults *r, int ascending)
{
    if (!r || r->count < 2)
        return;
    qsort(r->results, r->count, sizeof(CsQueryResult),
          ascending ? cmp_results : cmp_results_desc);
}

size_t sc_query_count(ScWorkbook *wb, const CsQuery *q)
{
    size_t cnt = 0;
    if (!wb || !q)
        return 0;
    if (scan_query(wb, q, visit_count, &cnt, NULL) != SC_OK)
        return 0;
    return cnt;
}

int sc_query_aggregate(ScWorkbook *wb, const CsQuery *q, int agg_type, double *out)
{
    AggCtx ac;

    if (!wb || !q || !out)
        return SC_ERR_INTERNAL;

    memset(&ac, 0, sizeof(ac));
    ac.agg_type = agg_type;
    ac.min = DBL_MAX;
    ac.max = -DBL_MAX;

    if (scan_query(wb, q, visit_agg, &ac, NULL) != SC_OK)
        return SC_ERR_INTERNAL;

    switch (agg_type) {
    case SC_AGG_SUM:
        *out = ac.sum;
        return SC_OK;
    case SC_AGG_AVG:
        if (ac.num_count == 0) {
            *out = 0.0;
            return SC_ERR_TYPE;
        }
        *out = ac.sum / (double)ac.num_count;
        return SC_OK;
    case SC_AGG_MIN:
        if (ac.num_count == 0) {
            *out = 0.0;
            return SC_ERR_TYPE;
        }
        *out = ac.min;
        return SC_OK;
    case SC_AGG_MAX:
        if (ac.num_count == 0) {
            *out = 0.0;
            return SC_ERR_TYPE;
        }
        *out = ac.max;
        return SC_OK;
    case SC_AGG_COUNT:
        *out = (double)ac.total;
        return SC_OK;
    default:
        return SC_ERR_RANGE;
    }
}

int sc_query_find_first(ScWorkbook *wb, const CsQuery *q,
                        int *sheet, int *row, int *col)
{
    FirstCtx fc;
    FirstWrap wrap;
    int rc, stop = 0;

    if (!wb || !q || !sheet || !row || !col)
        return SC_ERR_INTERNAL;

    memset(&fc, 0, sizeof(fc));
    wrap.fc = &fc;
    wrap.stop = &stop;
    rc = scan_query(wb, q, visit_first, &wrap, &stop);
    if (rc != SC_OK)
        return rc;
    if (!fc.found)
        return SC_ERR_BADREF;

    *sheet = fc.sheet;
    *row = fc.row;
    *col = fc.col;
    return SC_OK;
}

int sc_query_filter_range(ScWorkbook *wb, int sheet, int r0, int c0, int r1, int c1,
                          int (*pred)(const ScVal *, void *), void *ctx,
                          CsQueryResults *out)
{
    CsQuery q;
    PredCtx pc;
    int rc;

    if (!wb || !pred || !out)
        return SC_ERR_INTERNAL;
    if (sheet < 0 || sheet >= wb->nsheets)
        return SC_ERR_RANGE;

    sc_query_init(&q);
    q.sheet = sheet;
    q.r0 = r0;
    q.c0 = c0;
    q.r1 = r1;
    q.c1 = c1;

    pc.pred = pred;
    pc.ctx = ctx;
    pc.out = out;
    out->count = 0;

    rc = scan_query(wb, &q, visit_pred, &pc, NULL);
    return rc;
}
