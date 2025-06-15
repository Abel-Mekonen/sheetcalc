/*
 * io.c — file read/write, mmap, hex dump, base64
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

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/mman.h>
#endif

int sc_safe_add(size_t a, size_t b, size_t *out);
void cs_free_buf(uint8_t *data, size_t len);

enum { SC_IO_TAG_MAGIC = 0x4353494fu, SC_IO_MALLOC = 0, SC_IO_MMAP = 1 };

typedef struct ScIoTag {
    uint32_t magic, kind;
    size_t len, map_sz;
    uint8_t *payload;
    struct ScIoTag *link;
} ScIoTag;

static ScIoTag *g_mmap_tags;

static ScIoTag *tag_from_payload(uint8_t *p)
{
    ScIoTag *t = p ? (ScIoTag *)(p - sizeof(ScIoTag)) : NULL;
    return (t && t->magic == SC_IO_TAG_MAGIC) ? t : NULL;
}

static int io_alloc(size_t len, uint8_t **out, size_t *out_len)
{
    ScIoTag *tag;
    size_t total;
    if (!out || !out_len || !sc_safe_add(len, sizeof(ScIoTag), &total))
        return SC_ERR_IO;
    tag = (ScIoTag *)malloc(total);
    if (!tag)
        return SC_ERR_NOMEM;
    tag->magic = SC_IO_TAG_MAGIC;
    tag->kind = SC_IO_MALLOC;
    tag->len = len;
    tag->payload = NULL;
    tag->link = NULL;
    *out = (uint8_t *)(tag + 1);
    *out_len = len;
    return SC_OK;
}

int sc_read_file(const char *path, uint8_t **data, size_t *len)
{
    int fd;
    struct stat st;
    ssize_t nr;
    if (!path || !data || !len)
        return SC_ERR_IO;
    *data = NULL;
    *len = 0;
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size > SIZE_MAX) {
        if (fd >= 0)
            close(fd);
        return SC_ERR_IO;
    }
    if (io_alloc((size_t)st.st_size, data, len) != SC_OK) {
        close(fd);
        return SC_ERR_NOMEM;
    }
    nr = read(fd, *data, (size_t)st.st_size);
    close(fd);
    if (nr != (ssize_t)st.st_size) {
        cs_free_buf(*data, *len);
        *data = NULL;
        *len = 0;
        return SC_ERR_IO;
    }
    return SC_OK;
}

int cs_write_file(const char *path, const uint8_t *data, size_t len)
{
    int fd;
    ssize_t nw = 0;
    if (!path)
        return SC_ERR_IO;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return SC_ERR_IO;
    if (len > 0 && (!data || (nw = write(fd, data, len)) != (ssize_t)len)) {
        close(fd);
        return SC_ERR_IO;
    }
    close(fd);
    return SC_OK;
}

int cs_mmap_file(const char *path, uint8_t **data, size_t *len)
{
#if defined(__APPLE__) || defined(__linux__)
    int fd;
    struct stat st;
    ScIoTag *tag;
    void *map;
    if (!path || !data || !len)
        return SC_ERR_IO;
    *data = NULL;
    *len = 0;
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0 || (uint64_t)st.st_size > SIZE_MAX) {
        if (fd >= 0)
            close(fd);
        return sc_read_file(path, data, len);
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return sc_read_file(path, data, len);
    tag = (ScIoTag *)calloc(1, sizeof(*tag));
    if (!tag) {
        munmap(map, (size_t)st.st_size);
        return SC_ERR_NOMEM;
    }
    tag->magic = SC_IO_TAG_MAGIC;
    tag->kind = SC_IO_MMAP;
    tag->len = tag->map_sz = (size_t)st.st_size;
    tag->payload = (uint8_t *)map;
    tag->link = g_mmap_tags;
    g_mmap_tags = tag;
    *data = tag->payload;
    *len = tag->len;
    return SC_OK;
#else
    return sc_read_file(path, data, len);
#endif
}

void cs_free_buf(uint8_t *data, size_t len)
{
    ScIoTag *tag, **pp;
    (void)len;
    if (!data)
        return;
#if defined(__APPLE__) || defined(__linux__)
    for (tag = g_mmap_tags; tag; tag = tag->link) {
        if (tag->payload == data) {
            munmap(tag->payload, tag->map_sz);
            for (pp = &g_mmap_tags; *pp; pp = &(*pp)->link)
                if (*pp == tag) {
                    *pp = tag->link;
                    break;
                }
            free(tag);
            return;
        }
    }
#endif
    tag = tag_from_payload(data);
    if (tag && tag->kind == SC_IO_MALLOC) {
        free(tag);
        return;
    }
    free(data);
}

void cs_hexdump(const uint8_t *data, size_t len, FILE *out)
{
    size_t off = 0;
    if (!out)
        out = stdout;
    for (; data && off < len; off += 16) {
        size_t n = len - off, i;
        if (n > 16)
            n = 16;
        fprintf(out, "%08zx  ", off);
        for (i = 0; i < 16; i++) {
            if (i == 8)
                fputc(' ', out);
            if (i < n)
                fprintf(out, "%02x ", data[off + i]);
            else
                fprintf(out, "   ");
        }
        fputs(" |", out);
        for (i = 0; i < n; i++) {
            unsigned char c = data[off + i];
            fputc((c >= 32 && c < 127) ? (int)c : '.', out);
        }
        fputs("|\n", out);
    }
}

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_idx(int c)
{
    const char *p = strchr(b64, c);
    return p ? (int)(p - b64) : -1;
}

static void b64_emit(char *buf, size_t *o, uint32_t v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        buf[(*o)++] = b64[(v >> (18 - 6 * i)) & 63];
}

int cs_base64_encode(const uint8_t *data, size_t len, char **out, size_t *out_len)
{
    size_t i, o = 0;
    char *buf;
    if (!out || !out_len)
        return SC_ERR_IO;
    *out = NULL;
    *out_len = 0;
    if (!len) {
        *out = (char *)calloc(1, 1);
        return *out ? SC_OK : SC_ERR_NOMEM;
    }
    if (!data)
        return SC_ERR_IO;
    buf = (char *)malloc(((len + 2) / 3) * 4 + 1);
    if (!buf)
        return SC_ERR_NOMEM;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        b64_emit(buf, &o, v, 4);
    }
    if (i < len) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len)
            v |= (uint32_t)data[i + 1] << 8;
        b64_emit(buf, &o, v, 2);
        buf[o++] = (i + 1 < len) ? b64[(v >> 6) & 63] : '=';
        buf[o++] = '=';
    }
    buf[o] = '\0';
    *out = buf;
    *out_len = o;
    return SC_OK;
}

int cs_base64_decode(const char *in, size_t n, uint8_t **out, size_t *out_len)
{
    size_t i, o = 0;
    uint8_t *buf;
    if (!out || !out_len)
        return SC_ERR_IO;
    *out = NULL;
    *out_len = 0;
    if (!in || !n)
        return SC_OK;
    while (n && (in[n - 1] == '=' || in[n - 1] == '\n' || in[n - 1] == '\r'))
        n--;
    buf = (uint8_t *)malloc((n / 4) * 3 + 3);
    if (!buf)
        return SC_ERR_NOMEM;
    for (i = 0; i < n; i += 4) {
        int a = b64_idx(in[i]);
        int b = (i + 1 < n) ? b64_idx(in[i + 1]) : -1;
        int c = (i + 2 < n) ? b64_idx(in[i + 2]) : -1;
        int d = (i + 3 < n) ? b64_idx(in[i + 3]) : -1;
        uint32_t v;
        int pad = 0;
        if (a < 0 || b < 0)
            goto bad;
        v = ((uint32_t)a << 18) | ((uint32_t)b << 12);
        if (c >= 0)
            v |= (uint32_t)c << 6;
        else
            pad++;
        if (d >= 0)
            v |= (uint32_t)d;
        else
            pad++;
        buf[o++] = (uint8_t)(v >> 16);
        if (pad < 2)
            buf[o++] = (uint8_t)(v >> 8);
        if (!pad)
            buf[o++] = (uint8_t)v;
    }
    *out = buf;
    *out_len = o;
    return SC_OK;
bad:
    free(buf);
    return SC_ERR_FORMAT;
}
