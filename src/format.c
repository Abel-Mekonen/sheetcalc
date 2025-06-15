/*
 * format.c — cell display formatting (numbers, dates, refs)
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

/*
 * Internal formatters — used by formula.c, convert.c, sci_format_cell.
 *
 *   int sc_format_value(const ScVal *v, uint32_t fmt_id, ScBuf *sb);
 *   int sc_col_to_alpha(int col, char *buf, size_t sz);
 *   int cs_alpha_to_col(const char *s, size_t len);
 *   int sc_format_ref(int sheet, int row, int col, int abs_row, int abs_col, ScBuf *sb);
 */

static const char *MONTHS[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

enum {
    SC_FMT_GENERAL = 0, SC_FMT_NUM_00 = 1, SC_FMT_NUM_COMMA = 2,
    SC_FMT_PCT = 3, SC_FMT_DATE = 4, SC_FMT_TIME = 5, SC_FMT_DATETIME = 6,
    SC_FMT_TEXT = 7, SC_FMT_SCI = 8, SC_FMT_CURRENCY = 9
};

typedef struct {
    char prefix[8];
    int  use_group, dec_places, pct, sci, sci_places;
} NumFmt;

static int up(char c) { return (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c; }

/* Excel serial: day 1 = 1900-01-01. Lotus 1-2-3 leap-year bug — serial 60
 * is the nonexistent 1900-02-29; for serial >= 61 we subtract one day. */
/* TODO: 1904-date-system, locale month/day order */

static void serial_to_ymd(double serial, int *y, int *m, int *d)
{
    static const int mdays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int days = (int)floor(serial), yr = 1900, mo = 1, dy = 1, dim;

    if (days <= 0) { *y = 1899; *m = 12; *d = 30; return; }
    if (days == 60) { *y = 1900; *m = 2; *d = 29; return; }
    if (days > 60) days--;
    days--;
    while (days > 0) {
        dim = mdays[mo - 1];
        if (mo == 2 && ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0))
            dim = 29;
        if (days < dim) break;
        days -= dim;
        if (++mo > 12) { mo = 1; yr++; }
    }
    dy += days;
    *y = yr; *m = mo; *d = dy;
}

static void serial_to_hms(double serial, int *h, int *mn, int *s)
{
    double frac = serial - floor(serial);
    int tot;

    if (frac < 0) frac += 1.0;
    tot = (int)floor(frac * 86400.0 + 0.5);
    if (tot >= 86400) tot = 0;
    *h = tot / 3600; *mn = (tot / 60) % 60; *s = tot % 60;
}

static int format_date(double serial, const char *fmt, ScBuf *sb)
{
    int yr, mo, dy, h, mn, sec;
    const char *p;

    if (!fmt || !*fmt) fmt = "yyyy-mm-dd";
    serial_to_ymd(serial, &yr, &mo, &dy);
    serial_to_hms(serial, &h, &mn, &sec);

    for (p = fmt; *p; p++) {
        if (p[0]=='y' && p[1]=='y' && p[2]=='y' && p[3]=='y') {
            if (sci_buf_appendf(sb, "%04d", yr) != SC_OK) return SC_ERR_NOMEM;
            p += 3;
        } else if (p[0]=='m' && p[1]=='m' && p[2]=='m') {
            if (mo < 1 || mo > 12) return SC_ERR_FORMAT;
            if (sci_buf_append(sb, MONTHS[mo-1], 3) != SC_OK) return SC_ERR_NOMEM;
            p += 2;
        } else if (p[0]=='m' && p[1]=='m') {
            if (sci_buf_appendf(sb, "%02d", mo) != SC_OK) return SC_ERR_NOMEM;
            p += 1;
        } else if (p[0]=='d' && p[1]=='d') {
            if (sci_buf_appendf(sb, "%02d", dy) != SC_OK) return SC_ERR_NOMEM;
            p += 1;
        } else if ((p[0]=='h'||p[0]=='H') && p[1]==p[0]) {
            if (sci_buf_appendf(sb, "%02d", h) != SC_OK) return SC_ERR_NOMEM;
            p += 1;
        } else if (p[0]=='M' && p[1]=='M') {
            if (sci_buf_appendf(sb, "%02d", mn) != SC_OK) return SC_ERR_NOMEM;
            p += 1;
        } else if (p[0]=='s' && p[1]=='s') {
            if (sci_buf_appendf(sb, "%02d", sec) != SC_OK) return SC_ERR_NOMEM;
            p += 1;
        } else if (sci_buf_append(sb, p, 1) != SC_OK) {
            return SC_ERR_NOMEM;
        }
    }
    return SC_OK;
}

static void parse_num_fmt(const char *fmt, NumFmt *nf)
{
    const char *p, *dot;
    int n;

    memset(nf, 0, sizeof(*nf));
    if (!fmt || !*fmt) return;
    p = fmt;
    while (*p && !strchr("0#.", *p) && *p != '%')
        nf->prefix[(int)strlen(nf->prefix)] = *p++;
    if (strstr(p, ",##") || strstr(p, "#,##")) nf->use_group = 1;
    dot = strchr(p, '.');
    if (dot) {
        for (n = 0; dot[n+1]=='0' || dot[n+1]=='#'; n++) nf->dec_places = n + 1;
    } else {
        for (n = (int)strlen(p); n > 0; n--)
            if (p[n-1]=='0') { nf->dec_places = 1; break; }
    }
    if (strchr(p, '%')) nf->pct = 1;
    if (strstr(p, "E+0") || strstr(p, "E+00") || strstr(p, "e+0")) {
        nf->sci = 1;
        nf->sci_places = (strstr(p, "E+00") || strstr(p, "e+00")) ? 2 : 0;
    }
}

static void insert_commas(char *s)
{
    size_t len = strlen(s), i, j, ncomma, out, base = (s[0]=='-') ? 1 : 0;
    char buf[128];

    for (i = base; i < len && s[i] != '.'; i++) {}
    ncomma = (i - base - 1) / 3;
    if (ncomma == 0 || len >= sizeof(buf)) return;
    out = len + ncomma;
    j = out; buf[j] = '\0'; i = len;
    while (i > base) {
        buf[--j] = s[--i];
        if ((len - i) % 3 == 0 && i > base) buf[--j] = ',';
    }
    if (base) buf[--j] = '-';
    memcpy(s, buf + j, out - j + 1);
}

static int format_number(double val, const char *fmt, ScBuf *sb)
{
    NumFmt nf;
    char numbuf[96];
    double v;
    int neg, rc;

    if (isnan(val) || isinf(val)) return sci_buf_append(sb, "#NUM!", 5);
    parse_num_fmt(fmt, &nf);
    v = val; neg = (v < 0); if (neg) v = -v;
    if (nf.pct) v *= 100.0;

    if (nf.sci)
        snprintf(numbuf, sizeof numbuf, "%.*E", nf.sci_places, neg ? -v : v);
    else if (nf.dec_places > 0)
        snprintf(numbuf, sizeof numbuf, "%.*f", nf.dec_places, neg ? -v : v);
    else
        snprintf(numbuf, sizeof numbuf, "%.0f", neg ? -v : v);

    if (nf.use_group && !nf.sci) insert_commas(numbuf);
    if (nf.prefix[0] && (rc = sci_buf_append(sb, nf.prefix, strlen(nf.prefix))) != SC_OK)
        return rc;
    if ((rc = sci_buf_append(sb, numbuf, strlen(numbuf))) != SC_OK) return rc;
    if (nf.pct && (rc = sci_buf_append(sb, "%", 1)) != SC_OK) return rc;
    return SC_OK;
}

int sc_col_to_alpha(int col, char *buf, size_t sz)
{
    char tmp[16];
    int n = 0, v, i;

    if (!buf || sz == 0 || col < 0) return SC_ERR_RANGE;
    v = col + 1;
    do {
        tmp[n++] = (char)('A' + (v - 1) % 26);
        v = (v - 1) / 26;
    } while (v > 0 && n < (int)sizeof(tmp));
    if ((size_t)n >= sz) return SC_ERR_OVERFLOW;
    for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return SC_OK;
}

int cs_alpha_to_col(const char *s, size_t len)
{
    int v = 0;
    size_t i;

    if (!s || len == 0) return SC_ERR_FORMAT;
    for (i = 0; i < len; i++) {
        char c = (char)up(s[i]);
        if (c < 'A' || c > 'Z') return SC_ERR_FORMAT;
        v = v * 26 + (c - 'A' + 1);
    }
    return (v <= 0) ? SC_ERR_FORMAT : v - 1;
}

int sc_format_ref(int sheet, int row, int col, int abs_row, int abs_col, ScBuf *sb)
{
    char colbuf[16];
    int rc;

    if (sc_col_to_alpha(col, colbuf, sizeof colbuf) != SC_OK) return SC_ERR_RANGE;
    if (sheet >= 0 && (rc = sci_buf_appendf(sb, "Sheet%d!", sheet + 1)) != SC_OK)
        return rc;
    if (abs_col && (rc = sci_buf_append(sb, "$", 1)) != SC_OK) return rc;
    if ((rc = sci_buf_append(sb, colbuf, strlen(colbuf))) != SC_OK) return rc;
    if (abs_row && (rc = sci_buf_append(sb, "$", 1)) != SC_OK) return rc;
    return sci_buf_appendf(sb, "%d", row + 1);
}

static int looks_like_date(double n)
{
    return n >= 1.0 && n < 60000.0 && n == floor(n);
}

static int format_general(const ScVal *v, ScBuf *sb)
{
    switch (v->type) {
    case SC_VAL_NUM:
        if (looks_like_date(v->as.num))
            return format_date(v->as.num, "yyyy-mm-dd", sb);
        return format_number(v->as.num, "0.##########", sb);
    case SC_VAL_STR:
        return sci_buf_append(sb, v->as.str.s, v->as.str.len);
    case SC_VAL_BOOL:
        return sci_buf_append(sb, v->as.boolean ? "TRUE" : "FALSE",
                              v->as.boolean ? 4 : 5);
    case SC_VAL_ERR:
        return sci_buf_append(sb, "#ERR!", 5);
    case SC_VAL_EMPTY:
    case SC_VAL_NIL:
        return SC_OK;
    default:
        return SC_ERR_TYPE;
    }
}

int sc_format_value(const ScVal *v, uint32_t fmt_id, ScBuf *sb)
{
    double n;
    int rc;

    if (!v || !sb) return SC_ERR_INTERNAL;
    if (fmt_id == SC_FMT_TEXT && v->type == SC_VAL_STR)
        return sci_buf_append(sb, v->as.str.s, v->as.str.len);
    if (v->type != SC_VAL_NUM && fmt_id != SC_FMT_GENERAL)
        return format_general(v, sb);

    n = (v->type == SC_VAL_NUM) ? v->as.num : 0.0;
    switch (fmt_id) {
    case SC_FMT_GENERAL:  return format_general(v, sb);
    case SC_FMT_NUM_00:   return format_number(n, "0.00", sb);
    case SC_FMT_NUM_COMMA:return format_number(n, "#,##0", sb);
    case SC_FMT_PCT:      return format_number(n, "0%", sb);
    case SC_FMT_DATE:     return format_date(n, "yyyy-mm-dd", sb);
    case SC_FMT_TIME:     return format_date(n, "hh:mm:ss", sb);
    case SC_FMT_DATETIME:
        rc = format_date(n, "yyyy-mm-dd", sb);
        if (rc != SC_OK) return rc;
        rc = sci_buf_append(sb, " ", 1);
        return (rc != SC_OK) ? rc : format_date(n, "hh:mm:ss", sb);
    case SC_FMT_TEXT:
        return (v->type == SC_VAL_STR)
            ? sci_buf_append(sb, v->as.str.s, v->as.str.len)
            : format_number(n, "0", sb);
    case SC_FMT_SCI:      return format_number(n, "0.00E+00", sb);
    case SC_FMT_CURRENCY: return format_number(n, "$#,##0.00", sb);
    default:
        return format_general(v, sb); /* TODO: custom fmt from style table */
    }
}

int sci_format_cell(const ScCell *c, ScBuf *sb)
{
    if (!c || !sb) return SC_ERR_INTERNAL;
    (void)c->style_id; /* TODO: map style_id -> fmt_id */
    return sc_format_value(&c->value, 0, sb);
}
