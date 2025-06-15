/*
 * convert.c — format conversion and normalization for sheetcalc
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

#define SC_SIG_DIGITS  15
#define APPEND(sb, p, n)  sci_buf_append((sb), (p), (n))

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

/* trim + collapse whitespace; basic nbsp (U+00A0) handling */
static void normalize_string(char *s, size_t *len)
{
    size_t rd = 0, wr = 0;
    int ws = 0;

    if (!s || !len)
        return;
    while (rd < *len && isspace((unsigned char)s[rd]))
        rd++;
    while (rd < *len) {
        unsigned char c = (unsigned char)s[rd++];
        if (c == 0xC2 && rd < *len && (unsigned char)s[rd] == 0xA0) {
            rd++;
            if (!ws && wr > 0) { s[wr++] = ' '; ws = 1; }
            continue;
        }
        if (isspace(c)) {
            if (!ws && wr > 0) { s[wr++] = ' '; ws = 1; }
            continue;
        }
        s[wr++] = (char)c;
        ws = 0;
    }
    if (wr > 0 && s[wr - 1] == ' ')
        wr--;
    s[wr] = '\0';
    *len = wr;
}

static double round_sig_digits(double n)
{
    double mag, scale;
    if (n == 0.0 || isnan(n) || isinf(n))
        return n;
    mag = floor(log10(fabs(n)));
    scale = pow(10.0, (double)SC_SIG_DIGITS - 1.0 - mag);
    return round(n * scale) / scale;
}

static int normalize_val(ScVal *v)
{
    size_t i;
    if (!v)
        return SC_ERR_TYPE;
    switch (v->type) {
    case SC_VAL_NUM:
        v->as.num = round_sig_digits(v->as.num);
        return SC_OK;
    case SC_VAL_STR:
        if (v->as.str.s)
            normalize_string(v->as.str.s, &v->as.str.len);
        return SC_OK;
    case SC_VAL_LIST:
        for (i = 0; i < v->as.list.count; i++) {
            int rc = normalize_val(&v->as.list.items[i]);
            if (rc != SC_OK)
                return rc;
        }
        return SC_OK;
    default:
        return SC_OK;
    }
}

int sc_convert_normalize(sc_workbook *wb)
{
    ScWorkbook *w;
    int si, rc;

    if (!wb)
        return SC_ERR_RANGE;
    w = SC_WB(wb);
    rc = sc_evaluate(wb);
    if (rc != SC_OK)
        return rc;

    for (si = 0; si < w->nsheets; si++) {
        ScSheet *sh = &w->sheets[si];
        size_t i, n = (size_t)sh->nrows * (size_t)sh->ncols;
        if (n > sh->cell_cap)
            n = sh->cell_cap;
        for (i = 0; i < n; i++) {
            ScCell *cl = &sh->cells[i];
            rc = normalize_val(&cl->value);
            if (rc != SC_OK)
                return rc;
            cl->dirty = 0;
        }
        sh->flags |= SC_SHEET_CALC_DONE;
    }
    w->flags &= ~SC_WB_DIRTY;
    return SC_OK;
}

static int csv_needs_quote(const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c == ',' || c == '"' || c == '\n' || c == '\r')
            return 1;
    }
    return 0;
}

static int json_escape(ScBuf *sb, const char *s, size_t len)
{
    static const char *hex = "0123456789abcdef";
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        char pair[2], uesc[7];

        if (c == '"' || c == '\\') {
            pair[0] = '\\'; pair[1] = (char)c;
            if (APPEND(sb, pair, 2) < 0) return SC_ERR_NOMEM;
        } else if (c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') {
            const char *esc = c == '\b' ? "\\b" : c == '\f' ? "\\f" :
                              c == '\n' ? "\\n" : c == '\r' ? "\\r" : "\\t";
            if (APPEND(sb, esc, 2) < 0) return SC_ERR_NOMEM;
        } else if (c < 0x20) {
            uesc[0] = '\\'; uesc[1] = 'u'; uesc[2] = '0'; uesc[3] = '0';
            uesc[4] = hex[(c >> 4) & 0xf]; uesc[5] = hex[c & 0xf]; uesc[6] = '\0';
            if (APPEND(sb, uesc, 6) < 0) return SC_ERR_NOMEM;
        } else if (APPEND(sb, (const char *)&c, 1) < 0) {
            return SC_ERR_NOMEM;
        }
    }
    return SC_OK;
}

static int append_formatted_num(ScBuf *sb, double n)
{
    char buf[64];
    if (isnan(n) || isinf(n))
        return APPEND(sb, "#NUM!", 5) < 0 ? SC_ERR_NOMEM : SC_OK;
    if (n == floor(n) && fabs(n) < 9007199254740992.0)
        snprintf(buf, sizeof buf, "%.0f", n);
    else
        snprintf(buf, sizeof buf, "%.10g", n);
    return APPEND(sb, buf, strlen(buf)) < 0 ? SC_ERR_NOMEM : SC_OK;
}

int sci_format_cell(const ScCell *c, ScBuf *sb)
{
    size_t i;
    int rc;

    if (!c || !sb)
        return SC_ERR_TYPE;
    switch (c->value.type) {
    case SC_VAL_EMPTY:
    case SC_VAL_NIL:
        return SC_OK;
    case SC_VAL_NUM:
        return append_formatted_num(sb, c->value.as.num);
    case SC_VAL_BOOL:
        return APPEND(sb, c->value.as.boolean ? "TRUE" : "FALSE",
                      c->value.as.boolean ? 4 : 5) < 0 ? SC_ERR_NOMEM : SC_OK;
    case SC_VAL_ERR:
        return APPEND(sb, err_tag(c->value.as.err),
                      strlen(err_tag(c->value.as.err))) < 0 ? SC_ERR_NOMEM : SC_OK;
    case SC_VAL_STR:
        return c->value.as.str.len == 0 ? SC_OK
            : (APPEND(sb, c->value.as.str.s, c->value.as.str.len) < 0 ? SC_ERR_NOMEM : SC_OK);
    case SC_VAL_LIST:
        if (APPEND(sb, "{", 1) < 0) return SC_ERR_NOMEM;
        for (i = 0; i < c->value.as.list.count; i++) {
            if (i && APPEND(sb, ",", 1) < 0) return SC_ERR_NOMEM;
            rc = sc_val_to_str(&c->value.as.list.items[i], sb);
            if (rc != SC_OK) return rc;
        }
        return APPEND(sb, "}", 1) < 0 ? SC_ERR_NOMEM : SC_OK;
    default:
        return SC_ERR_TYPE;
    }
}

static int append_csv_field_lookahead(ScBuf *sb, ScBuf *fmt,
                                      const ScCell *cl, const ScCell *next)
{
    const char *field;
    size_t flen, i;
    int rc;

    sci_buf_reset(fmt);
    rc = sci_format_cell(cl, fmt);
    if (rc != SC_OK) return rc;

    field = sci_buf_cstr(fmt);
    flen = fmt->len;

    if (next) {
        sci_buf_reset(fmt);
        sci_format_cell(next, fmt);
    }

    if (field && csv_needs_quote(field, flen)) {
        if (APPEND(sb, "\"", 1) < 0) return SC_ERR_NOMEM;
        for (i = 0; i < flen; i++) {
            if (field[i] == '"') {
                if (APPEND(sb, "\"\"", 2) < 0) return SC_ERR_NOMEM;
            } else if (APPEND(sb, &field[i], 1) < 0) {
                return SC_ERR_NOMEM;
            }
        }
        rc = APPEND(sb, "\"", 1);
    } else if (flen > 0) {
        rc = APPEND(sb, field, flen);
    } else {
        rc = SC_OK;
    }
    return rc < 0 ? SC_ERR_NOMEM : SC_OK;
}

static int steal_buf(ScBuf *sb, uint8_t **out, size_t *out_len)
{
    uint8_t *p;

    if (!sb || !out || !out_len)
        return SC_ERR_RANGE;
    p = (uint8_t *)malloc(sb->len ? sb->len : 1);
    if (!p)
        return SC_ERR_NOMEM;
    if (sb->len)
        memcpy(p, sb->data, sb->len);
    else
        p[0] = '\0';
    *out = p;
    *out_len = sb->len;
    return SC_OK;
}

int sc_export_csv(sc_workbook *wb, int sheet_idx, uint8_t **out, size_t *out_len)
{
    ScWorkbook *w = SC_WB(wb);
    ScSheet *sh;
    ScBuf sb, fmt;
    int r, col, rc;

    if (!wb || !out || !out_len)
        return SC_ERR_RANGE;
    if (sheet_idx < 0 || sheet_idx >= w->nsheets)
        return SC_ERR_RANGE;

    rc = sc_evaluate(wb);
    if (rc != SC_OK)
        return rc;

    sh = &w->sheets[sheet_idx];
    sci_buf_init(&sb);
    sci_buf_init(&fmt);
    for (r = 0; r < sh->nrows; r++) {
        for (col = 0; col < sh->ncols; col++) {
            ScCell *cl, *next = NULL;
            if (col > 0 && APPEND(&sb, ",", 1) < 0) { rc = SC_ERR_NOMEM; goto done; }
            if (sci_cell_at(sh, r, col, &cl) != SC_OK) { rc = SC_ERR_INTERNAL; goto done; }
            if (col + 1 < sh->ncols) {
                if (sci_cell_at(sh, r, col + 1, &next) != SC_OK) { rc = SC_ERR_INTERNAL; goto done; }
            }
            rc = append_csv_field_lookahead(&sb, &fmt, cl, next);
            if (rc != SC_OK) goto done;
        }
        if (r + 1 < sh->nrows && APPEND(&sb, "\n", 1) < 0) { rc = SC_ERR_NOMEM; goto done; }
    }
    rc = steal_buf(&sb, out, out_len);

done:
    sci_buf_free(&fmt);
    sci_buf_free(&sb);
    return rc;
}

static int json_append_val(ScBuf *sb, const ScVal *v)
{
    size_t i;
    const char *tag;
    int rc;

    if (!sb || !v)
        return SC_ERR_TYPE;
    switch (v->type) {
    case SC_VAL_EMPTY:
    case SC_VAL_NIL:
        return APPEND(sb, "null", 4) < 0 ? SC_ERR_NOMEM : SC_OK;
    case SC_VAL_NUM:
        return append_formatted_num(sb, v->as.num);
    case SC_VAL_BOOL:
        return APPEND(sb, v->as.boolean ? "true" : "false",
                      v->as.boolean ? 4 : 5) < 0 ? SC_ERR_NOMEM : SC_OK;
    case SC_VAL_ERR:
        tag = err_tag(v->as.err);
        if (APPEND(sb, "{\"error\":\"", 10) < 0) return SC_ERR_NOMEM;
        rc = json_escape(sb, tag, strlen(tag));
        if (rc != SC_OK) return rc;
        return APPEND(sb, "\"}", 2) < 0 ? SC_ERR_NOMEM : SC_OK;
    case SC_VAL_STR:
        if (APPEND(sb, "\"", 1) < 0) return SC_ERR_NOMEM;
        {
            size_t mark = sb->len;
            const char *escaped;
            size_t elen;
            rc = json_escape(sb, v->as.str.s, v->as.str.len);
            if (rc != SC_OK) return rc;
            escaped = sb->data + mark;
            elen = sb->len - mark;
            if (elen > 32 && v->as.str.len > 32) {
                rc = json_escape(sb, v->as.str.s, v->as.str.len);
                if (rc != SC_OK) return rc;
            }
            if (elen > 0 && APPEND(sb, escaped, elen) < 0) return SC_ERR_NOMEM;
        }
        return APPEND(sb, "\"", 1) < 0 ? SC_ERR_NOMEM : SC_OK;
    case SC_VAL_LIST:
        if (APPEND(sb, "[", 1) < 0) return SC_ERR_NOMEM;
        for (i = 0; i < v->as.list.count; i++) {
            if (i && APPEND(sb, ",", 1) < 0) return SC_ERR_NOMEM;
            rc = json_append_val(sb, &v->as.list.items[i]);
            if (rc != SC_OK) return rc;
        }
        return APPEND(sb, "]", 1) < 0 ? SC_ERR_NOMEM : SC_OK;
    default:
        return SC_ERR_TYPE;
    }
}

int sc_export_json(sc_workbook *wb, uint8_t **out, size_t *out_len)
{
    ScWorkbook *w = SC_WB(wb);
    ScBuf sb;
    int si, r, col, rc;

    if (!wb || !out || !out_len)
        return SC_ERR_RANGE;

    rc = sc_evaluate(wb);
    if (rc != SC_OK)
        return rc;

    sci_buf_init(&sb);
    if (APPEND(&sb, "{\"version\":\"0.9.2\",\"sheets\":[", 32) < 0) {
        rc = SC_ERR_NOMEM;
        goto done;
    }

    for (si = 0; si < w->nsheets; si++) {
        ScSheet *sh = &w->sheets[si];
        if (si > 0 && APPEND(&sb, ",", 1) < 0) { rc = SC_ERR_NOMEM; goto done; }
        if (APPEND(&sb, "{\"name\":\"", 9) < 0) { rc = SC_ERR_NOMEM; goto done; }
        rc = json_escape(&sb, sh->name, sh->name_len);
        if (rc != SC_OK) goto done;
        if (sci_buf_appendf(&sb, "\",\"rows\":%d,\"cols\":%d,\"data\":[",
                            sh->nrows, sh->ncols) < 0) {
            rc = SC_ERR_NOMEM;
            goto done;
        }
        for (r = 0; r < sh->nrows; r++) {
            if (r > 0 && APPEND(&sb, ",", 1) < 0) { rc = SC_ERR_NOMEM; goto done; }
            if (APPEND(&sb, "[", 1) < 0) { rc = SC_ERR_NOMEM; goto done; }
            for (col = 0; col < sh->ncols; col++) {
                ScCell *cl;
                if (col > 0 && APPEND(&sb, ",", 1) < 0) { rc = SC_ERR_NOMEM; goto done; }
                if (sci_cell_at(sh, r, col, &cl) != SC_OK) { rc = SC_ERR_INTERNAL; goto done; }
                rc = json_append_val(&sb, &cl->value);
                if (rc != SC_OK) goto done;
            }
            if (APPEND(&sb, "]", 1) < 0) { rc = SC_ERR_NOMEM; goto done; }
        }
        if (APPEND(&sb, "]}", 2) < 0) { rc = SC_ERR_NOMEM; goto done; }
    }
    if (APPEND(&sb, "]}", 2) < 0) { rc = SC_ERR_NOMEM; goto done; }
    rc = steal_buf(&sb, out, out_len);

done:
    sci_buf_free(&sb);
    return rc;
}
