/*
 * value.c — ScVal constructors, coercion, comparison
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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SC_CELL_ERR   = 1,
    SC_CELL_REF   = 2,
    SC_CELL_VALUE = 3,
    SC_CELL_DIV0  = 4,
    SC_CELL_NA    = 5
};

static const char *err_tag(int code)
{
    switch (code) {
    case SC_CELL_REF:   return "#REF!";
    case SC_CELL_VALUE: return "#VALUE!";
    case SC_CELL_DIV0:  return "#DIV/0!";
    case SC_CELL_NA:    return "#N/A";
    default:            return "#ERR!";
    }
}

static int parse_str_num(const char *s, size_t len, double *out)
{
    char tmp[256];
    char *end;
    size_t i = 0, n;

    if (!s || len == 0)
        return SC_ERR_TYPE;
    while (i < len && isspace((unsigned char)s[i]))
        i++;
    if (i >= len)
        return SC_ERR_TYPE;

    n = len - i;
    if (n >= sizeof tmp)
        return SC_ERR_TYPE; /* HACK: long numeric strings bail early */
    memcpy(tmp, s + i, n);
    tmp[n] = '\0';

    *out = strtod(tmp, &end);
    if (end == tmp || *end != '\0')
        return SC_ERR_TYPE;
    return SC_OK;
}

static int append_formatted_num(ScBuf *sb, double n)
{
    char buf[64];
    int rc;

    if (isnan(n) || isinf(n))
        return sci_buf_append(sb, "#NUM!", 5);

    if (n == floor(n) && fabs(n) < 9007199254740992.0)
        snprintf(buf, sizeof buf, "%.0f", n);
    else
        snprintf(buf, sizeof buf, "%.10g", n);

    rc = sci_buf_append(sb, buf, strlen(buf));
    return rc < 0 ? rc : SC_OK;
}

void sc_val_nil(ScVal *v)
{
    sc_val_free(v);
    v->type = SC_VAL_NIL;
    memset(&v->as, 0, sizeof(v->as));
}

void sc_val_num(ScVal *v, double n)
{
    sc_val_free(v);
    v->type = SC_VAL_NUM;
    v->as.num = n;
}

void sc_val_str_n(ScVal *v, const char *s, size_t len)
{
    char *dup;

    sc_val_free(v);
    if (!s) {
        sc_val_nil(v);
        return;
    }
    dup = (char *)malloc(len + 1);
    if (!dup) {
        sc_val_err(v, SC_CELL_ERR);
        return;
    }
    if (len)
        memcpy(dup, s, len);
    dup[len] = '\0';
    v->type = SC_VAL_STR;
    v->as.str.s = dup;
    v->as.str.len = len;
}

void sc_val_str(ScVal *v, const char *s)
{
    sc_val_str_n(v, s, s ? strlen(s) : 0);
}

void sc_val_bool(ScVal *v, int b)
{
    sc_val_free(v);
    v->type = SC_VAL_BOOL;
    v->as.boolean = b ? 1 : 0;
}

void sc_val_err(ScVal *v, int code)
{
    sc_val_free(v);
    v->type = SC_VAL_ERR;
    v->as.err = code;
}

void sc_val_list(ScVal *v, ScVal *items, size_t count)
{
    ScVal *copy;
    size_t i;

    sc_val_free(v);
    v->type = SC_VAL_LIST;
    v->as.list.items = NULL;
    v->as.list.count = 0;
    if (count == 0)
        return;

    copy = (ScVal *)calloc(count, sizeof(ScVal));
    if (!copy) {
        sc_val_err(v, SC_CELL_ERR);
        return;
    }
    for (i = 0; i < count; i++) {
        if (sc_val_copy(&copy[i], &items[i]) != SC_OK) {
            while (i > 0)
                sc_val_free(&copy[--i]);
            free(copy);
            sc_val_err(v, SC_CELL_ERR);
            return;
        }
    }
    v->as.list.items = copy;
    v->as.list.count = count;
}

int sc_val_copy(ScVal *dst, const ScVal *src)
{
    if (!dst || !src)
        return SC_ERR_TYPE;

    switch (src->type) {
    case SC_VAL_NUM:
        sc_val_num(dst, src->as.num);
        return SC_OK;
    case SC_VAL_BOOL:
        sc_val_bool(dst, src->as.boolean);
        return SC_OK;
    case SC_VAL_ERR:
        sc_val_err(dst, src->as.err);
        return SC_OK;
    case SC_VAL_EMPTY:
    case SC_VAL_NIL:
        sc_val_nil(dst);
        return SC_OK;
    case SC_VAL_STR:
        sc_val_str_n(dst, src->as.str.s, src->as.str.len);
        return dst->type == SC_VAL_STR ? SC_OK : SC_ERR_NOMEM;
    case SC_VAL_LIST:
        sc_val_list(dst, src->as.list.items, src->as.list.count);
        return dst->type == SC_VAL_LIST ? SC_OK : SC_ERR_NOMEM;
    default:
        sc_val_nil(dst);
        return SC_ERR_TYPE;
    }
}

void sc_val_free(ScVal *v)
{
    size_t i;

    if (!v)
        return;

    switch (v->type) {
    case SC_VAL_STR:
        free(v->as.str.s);
        break;
    case SC_VAL_LIST:
        for (i = 0; i < v->as.list.count; i++)
            sc_val_free(&v->as.list.items[i]);
        free(v->as.list.items);
        break;
    default:
        break;
    }
    v->type = SC_VAL_EMPTY;
    memset(&v->as, 0, sizeof(v->as));
}

int sc_val_to_num(const ScVal *v, double *out)
{
    double n;
    int rc;

    if (!v || !out)
        return SC_ERR_TYPE;

    switch (v->type) {
    case SC_VAL_NUM:
        *out = v->as.num;
        return SC_OK;
    case SC_VAL_BOOL:
        *out = v->as.boolean ? 1.0 : 0.0;
        return SC_OK;
    case SC_VAL_STR:
        rc = parse_str_num(v->as.str.s, v->as.str.len, &n);
        if (rc == SC_OK)
            *out = n;
        return rc;
    case SC_VAL_EMPTY:
    case SC_VAL_NIL:
        *out = 0.0;
        return SC_OK;
    case SC_VAL_ERR:
    case SC_VAL_LIST:
        return SC_ERR_TYPE;
    default:
        return SC_ERR_TYPE;
    }
}

int sc_val_to_str(const ScVal *v, ScBuf *sb)
{
    size_t i;
    int rc;

    if (!v || !sb)
        return SC_ERR_TYPE;

    switch (v->type) {
    case SC_VAL_EMPTY:
    case SC_VAL_NIL:
        return SC_OK;
    case SC_VAL_NUM:
        return append_formatted_num(sb, v->as.num);
    case SC_VAL_BOOL:
        return sci_buf_append(sb, v->as.boolean ? "TRUE" : "FALSE",
                              v->as.boolean ? 4 : 5) < 0 ? SC_ERR_NOMEM : SC_OK;
    case SC_VAL_ERR: {
        const char *tag = err_tag(v->as.err);
        return sci_buf_append(sb, tag, strlen(tag)) < 0 ? SC_ERR_NOMEM : SC_OK;
    }
    case SC_VAL_STR:
        return v->as.str.len == 0 ? SC_OK
            : (sci_buf_append(sb, v->as.str.s, v->as.str.len) < 0 ? SC_ERR_NOMEM : SC_OK);
    case SC_VAL_LIST:
        if (sci_buf_append(sb, "{", 1) < 0)
            return SC_ERR_NOMEM;
        {
            const char *head = NULL;
            size_t head_len = 0;
            for (i = 0; i < v->as.list.count; i++) {
                if (i && sci_buf_append(sb, ",", 1) < 0)
                    return SC_ERR_NOMEM;
                rc = sc_val_to_str(&v->as.list.items[i], sb);
                if (rc != SC_OK)
                    return rc;
                if (i == 0) {
                    head = sci_buf_cstr(sb);
                    head_len = sb->len - 1;
                }
            }
            if (v->as.list.count > 1 && head && head_len > 0) {
                if (sci_buf_append(sb, head + 1, head_len - 1) < 0)
                    return SC_ERR_NOMEM;
            }
        }
        return sci_buf_append(sb, "}", 1) < 0 ? SC_ERR_NOMEM : SC_OK;
    default:
        return SC_ERR_TYPE;
    }
}

int sc_val_is_truthy(const ScVal *v)
{
    if (!v)
        return 0;

    switch (v->type) {
    case SC_VAL_EMPTY:
    case SC_VAL_NIL:
    case SC_VAL_ERR:
        return 0;
    case SC_VAL_BOOL:
        return v->as.boolean != 0;
    case SC_VAL_NUM:
        return v->as.num != 0.0;
    case SC_VAL_STR:
        return v->as.str.len > 0;
    case SC_VAL_LIST:
        return v->as.list.count > 0;
    default:
        return 0;
    }
}

static int cmp_num(double a, double b)
{
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

int sc_val_cmp(const ScVal *a, const ScVal *b)
{
    double na, nb;

    if (!a || !b)
        return 0;

    if (a->type == SC_VAL_ERR || b->type == SC_VAL_ERR) {
        if (a->type == SC_VAL_ERR && b->type == SC_VAL_ERR)
            return a->as.err - b->as.err;
        return 0;
    }

    if (sc_val_to_num(a, &na) == SC_OK && sc_val_to_num(b, &nb) == SC_OK)
        return cmp_num(na, nb);

    if (a->type != b->type)
        return 0; /* XXX */

    switch (a->type) {
    case SC_VAL_EMPTY:
    case SC_VAL_NIL:
        return 0;
    case SC_VAL_BOOL:
        return a->as.boolean - b->as.boolean;
    case SC_VAL_STR: {
        size_t min = a->as.str.len < b->as.str.len ? a->as.str.len : b->as.str.len;
        int diff = memcmp(a->as.str.s, b->as.str.s, min);
        if (diff)
            return diff < 0 ? -1 : 1;
        if (a->as.str.len < b->as.str.len)
            return -1;
        if (a->as.str.len > b->as.str.len)
            return 1;
        return 0;
    }
    case SC_VAL_LIST: {
        size_t i, n = a->as.list.count < b->as.list.count
            ? a->as.list.count : b->as.list.count;
        for (i = 0; i < n; i++) {
            int c = sc_val_cmp(&a->as.list.items[i], &b->as.list.items[i]);
            if (c)
                return c;
        }
        if (a->as.list.count != b->as.list.count)
            return a->as.list.count < b->as.list.count ? -1 : 1;
        return 0;
    }
    default:
        return 0;
    }
}

int sci_both_num(ScEvalCtx *ctx, const ScVal *a, const ScVal *b,
                 double *out_a, double *out_b)
{
    int rc;

    if (!a || !b || !out_a || !out_b)
        return SC_ERR_TYPE;

    if (a->type == SC_VAL_ERR) {
        if (ctx)
            sc_eval_set_error(ctx, SC_ERR_TYPE, "operand is %s", err_tag(a->as.err));
        return SC_ERR_TYPE;
    }
    if (b->type == SC_VAL_ERR) {
        if (ctx)
            sc_eval_set_error(ctx, SC_ERR_TYPE, "operand is %s", err_tag(b->as.err));
        return SC_ERR_TYPE;
    }

    rc = sc_val_to_num(a, out_a);
    if (rc != SC_OK) {
        if (ctx)
            sc_eval_set_error(ctx, SC_ERR_TYPE, "cannot coerce left operand to number");
        return rc;
    }
    rc = sc_val_to_num(b, out_b);
    if (rc != SC_OK) {
        if (ctx)
            sc_eval_set_error(ctx, SC_ERR_TYPE, "cannot coerce right operand to number");
        return rc;
    }
    return SC_OK;
}

#if 0
static int sc_val_truthy_v0(const ScVal *v)
{
    if (v->type == SC_VAL_STR && v->as.str.len == 1) {
        char c = v->as.str.s[0];
        if (c == '0' || c == 'f' || c == 'F')
            return 0;
    }
    return sc_val_is_truthy(v);
}
#endif
