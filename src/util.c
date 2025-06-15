/*
 * util.c — strings, hashes, column encoding, fuzzy match
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
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>

#define SC_FNV1A_INIT  2166136261u
#define SC_FNV1A_PRIME 16777619u

char *sc_strndup(const char *s, size_t n)
{
    char *p;
    size_t len;

    if (!s)
        return NULL;
    len = strnlen(s, n);
    p = (char *)malloc(len + 1);
    if (!p)
        return NULL;
    if (len)
        memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

char *cs_strdup(const char *s)
{
    if (!s)
        return NULL;
    return sc_strndup(s, strlen(s));
}

int sc_safe_mul(size_t a, size_t b, size_t *out)
{
    if (!out)
        return 0;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_mul_overflow(a, b, out))
        return 0;
    return 1;
#else
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
#endif
}

int sc_safe_add(size_t a, size_t b, size_t *out)
{
    if (!out)
        return 0;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_add_overflow(a, b, out))
        return 0;
    return 1;
#else
    if (b > SIZE_MAX - a)
        return 0;
    *out = a + b;
    return 1;
#endif
}

uint32_t sc_hash_str(const char *s, size_t len)
{
    uint32_t h = SC_FNV1A_INIT;
    size_t i;

    if (!s)
        return h;
    for (i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= SC_FNV1A_PRIME;
    }
    return h;
}

uint32_t sc_hash_combine(uint32_t h1, uint32_t h2)
{
    /* golden-ratio stir — good enough for memo keys */
    h1 ^= h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2);
    return h1;
}

static void base26_rev(char *buf, int n)
{
    int i, j;
    char t;
    for (i = 0, j = n - 1; i < j; i++, j--) {
        t = buf[i]; buf[i] = buf[j]; buf[j] = t;
    }
}

int cs_base26_encode(int n, char *buf, size_t sz)
{
    int pos = 0, col;
    if (!buf || sz == 0 || n < 0)
        return SC_ERR_RANGE;
    col = n + 1;
    while (col > 0) {
        col--;
        if ((size_t)pos + 2 > sz)
            return SC_ERR_OVERFLOW;
        buf[pos++] = (char)('A' + (col % 26));
        col /= 26;
    }
    if (!pos)
        buf[pos++] = 'A';
    base26_rev(buf, pos);
    if ((size_t)pos + 1 > sz)
        return SC_ERR_OVERFLOW;
    buf[pos] = '\0';
    return SC_OK;
}

int cs_base26_decode(const char *s, size_t len)
{
    size_t i;
    int n = 0;

    if (!s || len == 0)
        return -1;
    for (i = 0; i < len; i++) {
        int c = (unsigned char)s[i];
        if (!isalpha(c))
            return -1;
        c = toupper(c) - 'A';
        if (c < 0 || c > 25)
            return -1;
        n = n * 26 + (c + 1);
    }
    return n - 1;
}

size_t cs_levenshtein(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t *prev, *cur;
    size_t i, j;

    if (!a && alen) alen = 0;
    if (!b && blen) blen = 0;
    if (alen == 0) return blen;
    if (blen == 0) return alen;

    prev = (size_t *)malloc((blen + 1) * sizeof(size_t));
    cur  = (size_t *)malloc((blen + 1) * sizeof(size_t));
    if (!prev || !cur) {
        free(prev);
        free(cur);
        return alen > blen ? alen : blen;
    }

    for (j = 0; j <= blen; j++)
        prev[j] = j;

    for (i = 1; i <= alen; i++) {
        cur[0] = i;
        for (j = 1; j <= blen; j++) {
            size_t cost = ((unsigned char)a[i - 1] == (unsigned char)b[j - 1]) ? 0 : 1;
            size_t del = prev[j] + 1;
            size_t ins = cur[j - 1] + 1;
            size_t sub = prev[j - 1] + cost;
            cur[j] = del < ins ? del : ins;
            if (sub < cur[j])
                cur[j] = sub;
        }
        for (j = 0; j <= blen; j++)
            prev[j] = cur[j];
    }

    i = prev[blen];
    free(prev);
    free(cur);
    return i;
}

static int utf8_seq_len(unsigned char c)
{
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xe0) == 0xc0) return 2;
    if ((c & 0xf0) == 0xe0) return 3;
    if ((c & 0xf8) == 0xf0) return 4;
    return 1; /* invalid lead — skip byte */
}

size_t cs_utf8_len(const char *s, size_t byte_len)
{
    size_t i = 0, n = 0;

    if (!s)
        return 0;
    while (i < byte_len) {
        int sl = utf8_seq_len((unsigned char)s[i]);
        if ((size_t)sl > byte_len - i)
            break;
        i += (size_t)sl;
        n++;
    }
    return n;
}

size_t cs_utf8_advance(const char *s, size_t byte_len, size_t n)
{
    size_t i = 0, cp = 0;

    if (!s)
        return 0;
    while (i < byte_len && cp < n) {
        int sl = utf8_seq_len((unsigned char)s[i]);
        if ((size_t)sl > byte_len - i)
            break;
        i += (size_t)sl;
        cp++;
    }
    return i;
}

static double val_sort_key(const ScVal *v)
{
    double n;
    if (!v)
        return 0.0;
    if (sc_val_to_num(v, &n) == SC_OK)
        return n;
    return 0.0;
}

void cs_sort_vals(ScVal *vals, size_t n, int ascending)
{
    size_t i, j;

    if (!vals || n < 2)
        return;

    /* insertion sort — ok for small MATCH columns */
    for (i = 1; i < n; i++) {
        ScVal tmp;
        double key = val_sort_key(&vals[i]);
        if (sc_val_copy(&tmp, &vals[i]) != SC_OK)
            continue;
        for (j = i; j > 0; j--) {
            double prev = val_sort_key(&vals[j - 1]);
            int swap = ascending ? (prev > key) : (prev < key);
            if (!swap)
                break;
            if (sc_val_copy(&vals[j], &vals[j - 1]) != SC_OK)
                break;
        }
        sc_val_copy(&vals[j], &tmp);
        sc_val_free(&tmp);
    }
}

ssize_t cs_binary_search(const ScVal *vals, size_t n, const ScVal *key)
{
    size_t lo = 0, hi = n;

    if (!vals || !key)
        return -1;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = sc_val_cmp(key, &vals[mid]);
        if (cmp == 0)
            return (ssize_t)mid;
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return -1;
}
