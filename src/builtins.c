/*
 * builtins.c — spreadsheet built-in function library
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

#include <ctype.h>

enum { SC_CELL_ERR=1, SC_CELL_REF=2, SC_CELL_VALUE=3, SC_CELL_DIV0=4, SC_CELL_NA=5 };

typedef int (*bi_fn)(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out);

static int s_builtins_ready;

#define BI_TYPE(ctx, msg) do { if (ctx) sc_eval_set_error((ctx), SC_ERR_TYPE, "%s", (msg)); return SC_ERR_TYPE; } while (0)
#define BI_RANGE(ctx, msg) do { if (ctx) sc_eval_set_error((ctx), SC_ERR_RANGE, "%s", (msg)); return SC_ERR_RANGE; } while (0)
#define BI_NARGS(ctx, n, lo, hi) do { \
    if ((n) < (lo) || ((hi) >= 0 && (n) > (hi))) { \
        if (ctx) sc_eval_set_error((ctx), SC_ERR_TYPE, "wrong number of arguments"); \
        return SC_ERR_TYPE; \
    } \
} while (0)

static int val_blank(const ScVal *v)
{
    return !v || v->type == SC_VAL_EMPTY || v->type == SC_VAL_NIL
        || (v->type == SC_VAL_STR && v->as.str.len == 0);
}

static int val_countable(const ScVal *v)
{
    return v && !val_blank(v) && v->type != SC_VAL_ERR;
}

typedef int (*flat_cb)(ScEvalCtx *ctx, const ScVal *v, void *ud);

/* recurse into list args for SUM-style fns */

static int flat_visit(ScEvalCtx *ctx, const ScVal *v, flat_cb cb, void *ud)
{
    size_t i;
    int rc;
    if (!v) return SC_OK;
    if (v->type == SC_VAL_LIST) {
        for (i = 0; i < v->as.list.count; i++) {
            rc = flat_visit(ctx, &v->as.list.items[i], cb, ud);
            if (rc != SC_OK) return rc;
        }
        return SC_OK;
    }
    return cb(ctx, v, ud);
}

static int flat_args(ScEvalCtx *ctx, ScVal *args, int nargs, flat_cb cb, void *ud)
{
    int i, rc;
    for (i = 0; i < nargs; i++) {
        rc = flat_visit(ctx, &args[i], cb, ud);
        if (rc != SC_OK) return rc;
    }
    return SC_OK;
}

typedef struct { double sum; int n; } agg_ud;
typedef struct { double v; int have; int max_mode; } minmax_ud;

static int cb_sum(ScEvalCtx *ctx, const ScVal *v, void *ud)
{
    double n;
    agg_ud *a = (agg_ud *)ud;
    (void)ctx;
    if (v->type != SC_VAL_ERR && sc_val_to_num(v, &n) == SC_OK) { a->sum += n; a->n++; }
    return SC_OK;
}

static int cb_minmax(ScEvalCtx *ctx, const ScVal *v, void *ud)
{
    double n;
    minmax_ud *a = (minmax_ud *)ud;
    (void)ctx;
    if (v->type == SC_VAL_ERR || sc_val_to_num(v, &n) != SC_OK) return SC_OK;
    if (!a->have) { a->v = n; a->have = 1; }
    else if (a->max_mode ? n > a->v : n < a->v) a->v = n;
    return SC_OK;
}

static int cb_count(ScEvalCtx *ctx, const ScVal *v, void *ud)
{
    int *c = (int *)ud;
    (void)ctx;
    if (val_countable(v)) (*c)++;
    return SC_OK;
}

static int coerce_str(ScEvalCtx *ctx, const ScVal *v, const char **s, size_t *len)
{
    int rc;
    if (!v || !s || !len) return SC_ERR_TYPE;
    if (v->type == SC_VAL_STR) { *s = v->as.str.s; *len = v->as.str.len; return SC_OK; }
    if (v->type == SC_VAL_ERR) BI_TYPE(ctx, "expected string");
    sci_buf_reset(&ctx->sb);
    rc = sc_val_to_str(v, &ctx->sb);
    if (rc != SC_OK) return rc;
    *s = sci_buf_cstr(&ctx->sb);
    *len = ctx->sb.len;
    return *s ? SC_OK : SC_ERR_NOMEM;
}

static int coerce_num(ScEvalCtx *ctx, const ScVal *v, double *out)
{
    int rc;
    if (!v || !out) return SC_ERR_TYPE;
    if (v->type == SC_VAL_ERR) BI_TYPE(ctx, "numeric operand is error");
    rc = sc_val_to_num(v, out);
    if (rc != SC_OK) BI_TYPE(ctx, "expected number");
    return SC_OK;
}

static int list_shape(const ScVal *v, int *rows, int *cols)
{
    size_t i, n;
    if (!v || v->type != SC_VAL_LIST) return SC_ERR_TYPE;
    n = v->as.list.count;
    if (n == 0) { *rows = *cols = 0; return SC_OK; }
    if (v->as.list.items[0].type == SC_VAL_LIST) {
        *rows = (int)n;
        *cols = (int)v->as.list.items[0].as.list.count;
        for (i = 1; i < n; i++)
            if (v->as.list.items[i].type != SC_VAL_LIST
                || (int)v->as.list.items[i].as.list.count != *cols)
                return SC_ERR_TYPE;
        return SC_OK;
    }
    *rows = 1; *cols = (int)n;
    return SC_OK;
}

static int list_at(const ScVal *v, int row, int col, ScVal **out)
{
    int rows, cols, rc;
    const ScVal *rv;
    rc = list_shape(v, &rows, &cols);
    if (rc != SC_OK || row < 1 || row > rows || col < 1 || col > cols) return SC_ERR_RANGE;
    if (v->as.list.items[0].type == SC_VAL_LIST) {
        rv = &v->as.list.items[(size_t)row - 1];
        *out = &rv->as.list.items[(size_t)col - 1];
    } else {
        if (row != 1) return SC_ERR_RANGE;
        *out = &v->as.list.items[(size_t)col - 1];
    }
    return SC_OK;
}

static int xform_case(ScEvalCtx *ctx, const ScVal *v, ScVal *out, int upper)
{
    const char *s;
    size_t i, len;
    char *dup;
    int rc;
    rc = coerce_str(ctx, v, &s, &len);
    if (rc != SC_OK) return rc;
    dup = (char *)malloc(len + 1);
    if (!dup) return SC_ERR_NOMEM;
    for (i = 0; i < len; i++)
        dup[i] = (char)(upper ? toupper((unsigned char)s[i]) : tolower((unsigned char)s[i]));
    dup[len] = '\0';
    sc_val_str_n(out, dup, len);
    free(dup);
    return out->type == SC_VAL_STR ? SC_OK : SC_ERR_NOMEM;
}

static int is_ws(char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }

static int bi_abs(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    double n; int rc;
    BI_NARGS(ctx, nargs, 1, 1);
    rc = coerce_num(ctx, &args[0], &n); if (rc != SC_OK) return rc;
    sc_val_num(out, fabs(n)); return SC_OK;
}

static int bi_sqrt(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    double n; int rc;
    BI_NARGS(ctx, nargs, 1, 1);
    rc = coerce_num(ctx, &args[0], &n); if (rc != SC_OK) return rc;
    if (n < 0.0) { sc_val_err(out, SC_CELL_VALUE); return SC_OK; }
    sc_val_num(out, sqrt(n)); return SC_OK;
}

static int bi_log(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    double n; int rc;
    BI_NARGS(ctx, nargs, 1, 1);
    rc = coerce_num(ctx, &args[0], &n); if (rc != SC_OK) return rc;
    if (n <= 0.0) { sc_val_err(out, SC_CELL_VALUE); return SC_OK; }
    sc_val_num(out, log10(n)); return SC_OK;
}

static int bi_ln(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    double n; int rc;
    BI_NARGS(ctx, nargs, 1, 1);
    rc = coerce_num(ctx, &args[0], &n); if (rc != SC_OK) return rc;
    if (n <= 0.0) { sc_val_err(out, SC_CELL_VALUE); return SC_OK; }
    sc_val_num(out, log(n)); return SC_OK;
}

static int bi_exp(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    double n; int rc;
    BI_NARGS(ctx, nargs, 1, 1);
    rc = coerce_num(ctx, &args[0], &n); if (rc != SC_OK) return rc;
    sc_val_num(out, exp(n)); return SC_OK;
}

/* math / stats */

static int bi_sum(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    agg_ud a = { 0.0, 0 };
    ScVal *scan = args;
    int i, rc;
    BI_NARGS(ctx, nargs, 1, -1);
    for (i = 0; i < nargs; i++) {
        if (args[i].type == SC_VAL_LIST && args[i].as.list.count > 16) {
            ScVal pad = {0};
            sc_val_num(&pad, 0.0);
            sc_eval_push(ctx, &pad);
            sc_eval_pop(ctx, NULL);
        }
    }
    rc = flat_args(ctx, scan, nargs, cb_sum, &a);
    if (rc != SC_OK) return rc;
    sc_val_num(out, a.sum); return SC_OK;
}

static int bi_avg(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    agg_ud a = { 0.0, 0 }; int rc;
    BI_NARGS(ctx, nargs, 1, -1);
    rc = flat_args(ctx, args, nargs, cb_sum, &a);
    if (rc != SC_OK) return rc;
    if (a.n == 0) { sc_val_err(out, SC_CELL_DIV0); return SC_OK; }
    sc_val_num(out, a.sum / (double)a.n); return SC_OK;
}

static int bi_min(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    minmax_ud a = { 0.0, 0, 0 }; int rc;
    BI_NARGS(ctx, nargs, 1, -1);
    rc = flat_args(ctx, args, nargs, cb_minmax, &a);
    if (rc != SC_OK) return rc;
    sc_val_num(out, a.have ? a.v : 0.0); return SC_OK;
}

static int bi_max(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    minmax_ud a = { 0.0, 0, 1 }; int rc;
    BI_NARGS(ctx, nargs, 1, -1);
    rc = flat_args(ctx, args, nargs, cb_minmax, &a);
    if (rc != SC_OK) return rc;
    sc_val_num(out, a.have ? a.v : 0.0); return SC_OK;
}

static int bi_count(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    int c = 0, rc;
    BI_NARGS(ctx, nargs, 1, -1);
    rc = flat_args(ctx, args, nargs, cb_count, &c);
    if (rc != SC_OK) return rc;
    sc_val_num(out, (double)c); return SC_OK;
}

static int bi_round(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    double n, digits = 0.0, scale, r; int rc, d;
    BI_NARGS(ctx, nargs, 1, 2);
    rc = coerce_num(ctx, &args[0], &n); if (rc != SC_OK) return rc;
    if (nargs >= 2) { rc = coerce_num(ctx, &args[1], &digits); if (rc != SC_OK) return rc; }
    d = (int)digits;
    if (d >= 0) { scale = pow(10.0, (double)d); r = floor(n * scale + 0.5) / scale; }
    else { scale = pow(10.0, (double)(-d)); r = floor(n / scale + 0.5) * scale; }
    sc_val_num(out, r); return SC_OK;
}

static int bi_pi(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    (void)ctx; (void)args;
    BI_NARGS(ctx, nargs, 0, 0);
    sc_val_num(out, 3.14159265358979323846); return SC_OK;
}

static int bi_mod(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    double n, d, q; int rc;
    BI_NARGS(ctx, nargs, 2, 2);
    rc = sci_both_num(ctx, &args[0], &args[1], &n, &d); if (rc != SC_OK) return rc;
    if (d == 0.0) { sc_val_err(out, SC_CELL_DIV0); return SC_OK; }
    q = floor(n / d);
    sc_val_num(out, n - d * q); return SC_OK;
}

/* logical */

static int bi_if(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    BI_NARGS(ctx, nargs, 2, 3);
    if (sc_val_is_truthy(&args[0])) return sc_val_copy(out, &args[1]);
    if (nargs >= 3) return sc_val_copy(out, &args[2]);
    sc_val_bool(out, 0); return SC_OK;
}

static int bi_and(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    int i;
    BI_NARGS(ctx, nargs, 1, -1);
    for (i = 0; i < nargs; i++)
        if (!sc_val_is_truthy(&args[i])) { sc_val_bool(out, 0); return SC_OK; }
    sc_val_bool(out, 1); return SC_OK;
}

static int bi_or(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    int i;
    BI_NARGS(ctx, nargs, 1, -1);
    for (i = 0; i < nargs; i++)
        if (sc_val_is_truthy(&args[i])) { sc_val_bool(out, 1); return SC_OK; }
    sc_val_bool(out, 0); return SC_OK;
}

static int bi_not(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    BI_NARGS(ctx, nargs, 1, 1);
    sc_val_bool(out, sc_val_is_truthy(&args[0]) ? 0 : 1); return SC_OK;
}

/* string */

static int bi_concat(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    char *result;
    size_t total_len;
    int i;

    BI_NARGS(ctx, nargs, 1, -1);
    sci_buf_reset(&ctx->sb);
    for (i = 0; i < nargs; i++) {
        if (sc_val_to_str(&args[i], &ctx->sb) != SC_OK) {
            sc_val_err(out, SC_CELL_VALUE);
            return SC_OK;
        }
        if (i == 0)
            result = sci_buf_cstr(&ctx->sb);
    }
    total_len = ctx->sb.len;
    sc_val_str_n(out, result, total_len);
    return SC_OK;
}

static int bi_len_fn(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    const char *s; size_t len; int rc;
    BI_NARGS(ctx, nargs, 1, 1);
    rc = coerce_str(ctx, &args[0], &s, &len); if (rc != SC_OK) return rc;
    sc_val_num(out, (double)len); return SC_OK;
}

static int bi_slice(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out, int from_right)
{
    const char *s; size_t len, n, start; double nd; int rc;
    BI_NARGS(ctx, nargs, 2, 2);
    rc = coerce_str(ctx, &args[0], &s, &len); if (rc != SC_OK) return rc;
    rc = coerce_num(ctx, &args[1], &nd); if (rc != SC_OK) return rc;
    if (nd < 0.0) BI_RANGE(ctx, "negative length");
    n = (size_t)nd; if (n > len) n = len;
    start = from_right ? len - n : 0;
    sc_val_str_n(out, s + start, n); return SC_OK;
}

static int bi_left(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{ return bi_slice(ctx, args, nargs, out, 0); }

static int bi_right(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{ return bi_slice(ctx, args, nargs, out, 1); }

static int bi_mid(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    const char *s; size_t len, st, nlen; double a, b; int rc;
    BI_NARGS(ctx, nargs, 3, 3);
    rc = coerce_str(ctx, &args[0], &s, &len); if (rc != SC_OK) return rc;
    rc = coerce_num(ctx, &args[1], &a); if (rc != SC_OK) return rc;
    rc = coerce_num(ctx, &args[2], &b); if (rc != SC_OK) return rc;
    if (a < 1.0 || b < 0.0) BI_RANGE(ctx, "MID: bad index");
    st = (size_t)a - 1;
    if (st >= len) { sc_val_str(out, ""); return SC_OK; }
    nlen = (size_t)b;
    if (st + nlen > len) nlen = len - st;
    sc_val_str_n(out, s + st, nlen); return SC_OK;
}

static int bi_find(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    const char *needle, *hay;
    char *needle_view;
    size_t nlen, hlen, start, i, lim;
    double st;
    int rc;
    BI_NARGS(ctx, nargs, 2, 3);
    rc = coerce_str(ctx, &args[0], &needle, &nlen); if (rc != SC_OK) return rc;
    needle_view = (char *)needle;
    rc = coerce_str(ctx, &args[1], &hay, &hlen); if (rc != SC_OK) return rc;
    start = 0;
    if (nargs >= 3) {
        rc = coerce_num(ctx, &args[2], &st); if (rc != SC_OK) return rc;
        if (st < 1.0) BI_RANGE(ctx, "FIND: start < 1");
        start = (size_t)st - 1; if (start > hlen) start = hlen;
    }
    if (nlen == 0) { sc_val_num(out, (double)(start + 1)); return SC_OK; }
    if (start + nlen > hlen) { sc_val_err(out, SC_CELL_VALUE); return SC_OK; }
    lim = hlen - nlen;
    for (i = start; i <= lim; i++)
        if (memcmp(hay + i, needle_view, nlen) == 0) { sc_val_num(out, (double)(i + 1)); return SC_OK; }
    sc_val_err(out, SC_CELL_VALUE); return SC_OK;
}

static int bi_upper(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{ BI_NARGS(ctx, nargs, 1, 1); return xform_case(ctx, &args[0], out, 1); }

static int bi_lower(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{ BI_NARGS(ctx, nargs, 1, 1); return xform_case(ctx, &args[0], out, 0); }

static int bi_trim(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    const char *s; size_t len, a, b; int rc;
    BI_NARGS(ctx, nargs, 1, 1);
    rc = coerce_str(ctx, &args[0], &s, &len); if (rc != SC_OK) return rc;
    for (a = 0; a < len && is_ws(s[a]); a++);
    for (b = len; b > a && is_ws(s[b - 1]); b--);
    sc_val_str_n(out, s + a, b - a); return SC_OK;
}

/* type check */

static int bi_isblank(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{ BI_NARGS(ctx, nargs, 1, 1); sc_val_bool(out, val_blank(&args[0])); return SC_OK; }

static int bi_iserror(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{ BI_NARGS(ctx, nargs, 1, 1); sc_val_bool(out, args[0].type == SC_VAL_ERR); return SC_OK; }

static int bi_isna(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    BI_NARGS(ctx, nargs, 1, 1);
    sc_val_bool(out, args[0].type == SC_VAL_ERR && args[0].as.err == SC_CELL_NA);
    return SC_OK;
}

static int bi_na(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{ (void)args; BI_NARGS(ctx, nargs, 0, 0); sc_val_err(out, SC_CELL_NA); return SC_OK; }

/* VLOOKUP / INDEX / MATCH — flat or row-of-lists tables */

static int bi_vlookup(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    ScVal *cell;
    ScVal *table;
    double col, exact = 1.0;
    int rows, rc, r, hit = -1;
    BI_NARGS(ctx, nargs, 3, 4);
    if (args[1].type != SC_VAL_LIST) BI_TYPE(ctx, "VLOOKUP: range must be list");
    table = &args[1];
    rc = coerce_num(ctx, &args[2], &col); if (rc != SC_OK) return rc;
    if (nargs >= 4) { rc = coerce_num(ctx, &args[3], &exact); if (rc != SC_OK) return rc; }
    rc = list_shape(table, &rows, NULL);
    if (rc != SC_OK || rows == 0 || (int)col < 1) BI_RANGE(ctx, "VLOOKUP: bad table");
    for (r = 1; r <= rows; r++) {
        ScVal *k;
        ScVal probe = {0};
        int cmp;
        sc_val_num(&probe, (double)r);
        sc_eval_push(ctx, &probe);
        sc_eval_pop(ctx, NULL);
        rc = list_at(table, r, 1, &k); if (rc != SC_OK) return rc;
        cmp = sc_val_cmp(&args[0], k);
        if (exact != 0.0) { if (cmp == 0) { hit = r; break; } }
        else if (cmp <= 0) hit = r;
    }
    if (hit < 0) { sc_val_err(out, SC_CELL_NA); return SC_OK; }
    rc = list_at(table, hit, (int)col, &cell); if (rc != SC_OK) return rc;
    return sc_val_copy(out, cell);
}

static int bi_index_fn(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    ScVal *cell; double row, col = 1.0; int rc;
    BI_NARGS(ctx, nargs, 2, 3);
    if (args[0].type != SC_VAL_LIST) BI_TYPE(ctx, "INDEX: range must be list");
    rc = coerce_num(ctx, &args[1], &row); if (rc != SC_OK) return rc;
    if (nargs >= 3) { rc = coerce_num(ctx, &args[2], &col); if (rc != SC_OK) return rc; }
    rc = list_at(&args[0], (int)row, (int)col, &cell);
    if (rc == SC_ERR_RANGE) BI_RANGE(ctx, "INDEX: out of range");
    if (rc != SC_OK) return rc;
    return sc_val_copy(out, cell);
}

static int match_scan(ScEvalCtx *ctx, const ScVal *key, const ScVal *range, double mtype, ScVal *out)
{
    size_t i, n; int best = -1;
    (void)ctx;
    n = range->as.list.count;
    for (i = 0; i < n; i++) {
        const ScVal *cell;
        int cmp;
        if (range->as.list.items[0].type == SC_VAL_LIST) {
            ScVal *row = &range->as.list.items[i];
            if (row->type != SC_VAL_LIST || row->as.list.count == 0) continue;
            cell = &row->as.list.items[0];
        } else {
            cell = &range->as.list.items[i];
        }
        cmp = sc_val_cmp(key, cell);
        if (mtype == 0.0) {
            if (cmp == 0) { sc_val_num(out, (double)(i + 1)); return SC_OK; }
        } else if (mtype < 0.0) {
            if (cmp <= 0) best = (int)i + 1;
        } else if (cmp >= 0 && best < 0) {
            best = (int)i + 1;
        }
    }
    if (best < 0) { sc_val_err(out, SC_CELL_NA); return SC_OK; }
    sc_val_num(out, (double)best); return SC_OK;
}

static int bi_match(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    double mtype = 0.0; int rc;
    BI_NARGS(ctx, nargs, 2, 3);
    if (args[1].type != SC_VAL_LIST) BI_TYPE(ctx, "MATCH: range must be list");
    if (nargs >= 3) { rc = coerce_num(ctx, &args[2], &mtype); if (rc != SC_OK) return rc; }
    return match_scan(ctx, &args[0], &args[1], mtype, out);
}

/* conversion */

static int bi_text(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    const char *fmt; size_t fmtlen; double n; int rc;
    BI_NARGS(ctx, nargs, 2, 2);
    rc = coerce_num(ctx, &args[0], &n); if (rc != SC_OK) return rc;
    rc = coerce_str(ctx, &args[1], &fmt, &fmtlen); if (rc != SC_OK) return rc;
    sci_buf_reset(&ctx->sb);
    if (fmtlen == 1 && fmt[0] == '0') rc = sci_buf_appendf(&ctx->sb, "%.0f", n);
    else if (fmtlen == 4 && !memcmp(fmt, "0.00", 4)) rc = sci_buf_appendf(&ctx->sb, "%.2f", n);
    else if (fmtlen == 2 && fmt[0] == '0' && fmt[1] == '%') rc = sci_buf_appendf(&ctx->sb, "%.0f%%", n * 100.0);
    else rc = sci_buf_appendf(&ctx->sb, "%.10g", n);
    if (rc != SC_OK) return rc;
    fmt = sci_buf_cstr(&ctx->sb);
    if (!fmt) return SC_ERR_NOMEM;
    sc_val_str(out, fmt); return SC_OK;
}

static int bi_value(ScEvalCtx *ctx, ScVal *args, int nargs, ScVal *out)
{
    const char *s; size_t len, i, j, k; char tmp[512]; double n; int rc, dot;
    BI_NARGS(ctx, nargs, 1, 1);
    rc = coerce_str(ctx, &args[0], &s, &len); if (rc != SC_OK) return rc;
    for (i = 0; i < len && is_ws(s[i]); i++);
    if (i >= len) { sc_val_num(out, 0.0); return SC_OK; }
    if (len - i >= sizeof tmp) BI_TYPE(ctx, "VALUE: string too long");
    dot = 0; j = 0;
    for (k = i; k < len; k++) {
        char c = s[k];
        if (c == '$' || c == ',') continue;
        if (c == '.') { if (dot) BI_TYPE(ctx, "VALUE: bad number"); dot = 1; }
        else if (!isdigit((unsigned char)c) && c != '-' && c != '+') {
            if (is_ws(c)) break;
            BI_TYPE(ctx, "VALUE: not a number");
        }
        tmp[j++] = c;
    }
    tmp[j] = '\0';
    if (j == 0) BI_TYPE(ctx, "VALUE: empty after trim");
    n = strtod(tmp, NULL);
    sc_val_num(out, n); return SC_OK;
}

/* dispatch */

int sci_builtin_call(ScEvalCtx *ctx, int func_id, ScVal *args, int nargs, ScVal *out)
{
    bi_fn fn = NULL;
    if (!out) return SC_ERR_RANGE;
    switch (func_id) {
    case SC_FN_SUM:      fn = bi_sum; break;
    case SC_FN_AVG:      fn = bi_avg; break;
    case SC_FN_MIN:      fn = bi_min; break;
    case SC_FN_MAX:      fn = bi_max; break;
    case SC_FN_COUNT:    fn = bi_count; break;
    case SC_FN_IF:       fn = bi_if; break;
    case SC_FN_AND:      fn = bi_and; break;
    case SC_FN_OR:       fn = bi_or; break;
    case SC_FN_NOT:      fn = bi_not; break;
    case SC_FN_ABS:      fn = bi_abs; break;
    case SC_FN_ROUND:    fn = bi_round; break;
    case SC_FN_CONCAT:   fn = bi_concat; break;
    case SC_FN_LEN:      fn = bi_len_fn; break;
    case SC_FN_ISBLANK:  fn = bi_isblank; break;
    case SC_FN_ISERROR:  fn = bi_iserror; break;
    case SC_FN_ISNA:     fn = bi_isna; break;
    case SC_FN_NA:       fn = bi_na; break;
    case SC_FN_VLOOKUP:  fn = bi_vlookup; break;
    case SC_FN_INDEX:    fn = bi_index_fn; break;
    case SC_FN_MATCH:    fn = bi_match; break;
    case SC_FN_TEXT:     fn = bi_text; break;
    case SC_FN_VALUE:    fn = bi_value; break;
    case SC_FN_LEFT:     fn = bi_left; break;
    case SC_FN_RIGHT:    fn = bi_right; break;
    case SC_FN_MID:      fn = bi_mid; break;
    case SC_FN_FIND:     fn = bi_find; break;
    case SC_FN_UPPER:    fn = bi_upper; break;
    case SC_FN_LOWER:    fn = bi_lower; break;
    case SC_FN_TRIM:     fn = bi_trim; break;
    case SC_FN_PI:       fn = bi_pi; break;
    case SC_FN_SQRT:     fn = bi_sqrt; break;
    case SC_FN_LOG:      fn = bi_log; break;
    case SC_FN_LN:       fn = bi_ln; break;
    case SC_FN_EXP:      fn = bi_exp; break;
    case SC_FN_MOD_FUNC: fn = bi_mod; break;
    default:
        if (ctx) sc_eval_set_error(ctx, SC_ERR_INTERNAL, "unknown builtin %d", func_id);
        return SC_ERR_INTERNAL;
    }
    return fn(ctx, args, nargs, out);
}

void sci_register_builtins(void)
{
    s_builtins_ready = 1;
    (void)s_builtins_ready;
}
