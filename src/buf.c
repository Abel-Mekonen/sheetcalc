/*
 * buf.c — growable string builder for sheetcalc
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

#include <stdarg.h>

/* grow to hold at least min_cap content bytes (+1 for trailing NUL) */
static int buf_grow(ScBuf *b, size_t min_cap)
{
    size_t nc;
    char *p;

    if (min_cap <= b->cap)
        return SC_OK;

    nc = b->cap ? b->cap * 2 : SC_BUF_INIT_CAP;
    if (nc < min_cap)
        nc = min_cap;

    p = (char *)realloc(b->data, nc + 1);
    if (!p)
        return SC_ERR_NOMEM;
    b->data = p;
    b->cap = nc;
    return SC_OK;
}

void sci_buf_init(ScBuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int sci_buf_append(ScBuf *b, const char *s, size_t len)
{
    if (len == 0)
        return SC_OK;
    if (!s)
        return SC_ERR_NOMEM;

    if (buf_grow(b, b->len + len) != SC_OK)
        return SC_ERR_NOMEM;

    memcpy(b->data + b->len, s, len);
    b->len += len;
    return SC_OK;
}

int sci_buf_appendf(ScBuf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t room, need;

    va_start(ap, fmt);

    /* fast path: spare room in current allocation */
    if (b->cap > b->len) {
        room = b->cap - b->len;
        n = vsnprintf(b->data + b->len, room + 1, fmt, ap);
        if (n >= 0 && (size_t)n <= room) {
            b->len += (size_t)n;
            va_end(ap);
            return SC_OK;
        }
    }

    va_end(ap);
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return SC_ERR_NOMEM;

    need = b->len + (size_t)n;
    if (buf_grow(b, need) != SC_OK)
        return SC_ERR_NOMEM;

    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len = need;
    return SC_OK;
}

void sci_buf_reset(ScBuf *b)
{
    b->len = 0;
}

void sci_buf_free(ScBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

char *sci_buf_cstr(ScBuf *b)
{
    if (buf_grow(b, b->len) != SC_OK)
        return NULL;
    b->data[b->len] = '\0';
    /* pointer invalidated by the next append/appendf/reset/free */
    return b->data;
}
