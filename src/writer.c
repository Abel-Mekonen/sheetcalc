/*
 * writer.c — .scb (CellScript Binary) format encoder
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

#include <limits.h>

#define CSB_HDR_SZ      16u
#define CSB_SEC_ENT_SZ  12u
#define CHK(expr)       do { int _rc = (expr); if (_rc != SC_OK) return _rc; } while (0)
#define CHK_G(expr)     do { rc = (expr); if (rc != SC_OK) goto done; } while (0)

enum {
    SC_SEC_SHEETS = 1, SC_SEC_CELLS, SC_SEC_FORMULAS, SC_SEC_STRINGS,
    SC_SEC_STYLES, SC_SEC_NAMES, SC_SEC_METADATA
};

typedef struct { uint32_t type; size_t off_pos; } CsSecSlot;

typedef struct { char **items; uint32_t count, cap; } CsStrTab;
typedef struct { ScFormula **items; int count, cap; } CsFmlTab;

static int write_u8(ScBuf *b, uint8_t v)
{
    return sci_buf_append(b, (const char *)&v, 1) < 0 ? SC_ERR_NOMEM : SC_OK;
}

static int write_u16(ScBuf *b, uint16_t v)
{
    uint8_t le[2] = { (uint8_t)(v & 0xffu), (uint8_t)((v >> 8) & 0xffu) };
    return sci_buf_append(b, (const char *)le, 2) < 0 ? SC_ERR_NOMEM : SC_OK;
}

static int write_u32(ScBuf *b, uint32_t v)
{
    uint8_t le[4] = {
        (uint8_t)(v & 0xffu), (uint8_t)((v >> 8) & 0xffu),
        (uint8_t)((v >> 16) & 0xffu), (uint8_t)((v >> 24) & 0xffu)
    };
    return sci_buf_append(b, (const char *)le, 4) < 0 ? SC_ERR_NOMEM : SC_OK;
}

static int write_i32(ScBuf *b, int32_t v) { return write_u32(b, (uint32_t)v); }

static int write_f64(ScBuf *b, double v)
{
    union { uint64_t u; double d; } x;
    x.d = v;
    CHK(write_u32(b, (uint32_t)(x.u & 0xffffffffu)));
    return write_u32(b, (uint32_t)(x.u >> 32));
}

static int write_bytes(ScBuf *b, const void *data, size_t len)
{
    if (len == 0) return SC_OK;
    if (!data) return SC_ERR_NOMEM;
    return sci_buf_append(b, (const char *)data, len) < 0 ? SC_ERR_NOMEM : SC_OK;
}

static int patch_u32(ScBuf *b, size_t pos, uint32_t v)
{
    if (pos + 4 > b->len) return SC_ERR_INTERNAL;
    b->data[pos]     = (char)(v & 0xffu);
    b->data[pos + 1] = (char)((v >> 8) & 0xffu);
    b->data[pos + 2] = (char)((v >> 16) & 0xffu);
    b->data[pos + 3] = (char)((v >> 24) & 0xffu);
    return SC_OK;
}

static int cell_has_data(const ScCell *cl)
{
    if (!cl) return 0;
    if (cl->formula || cl->style_id) return 1;
    return cl->value.type != SC_VAL_EMPTY && cl->value.type != SC_VAL_NIL;
}

static int write_cell_value(ScBuf *b, const ScVal *v)
{
    uint8_t ty;

    if (!v) return SC_ERR_INTERNAL;
    ty = (uint8_t)v->type;
    if (ty == SC_VAL_NIL) ty = SC_VAL_EMPTY;
    CHK(write_u8(b, ty));
    switch (ty) {
    case SC_VAL_NUM:  return write_f64(b, v->as.num);
    case SC_VAL_STR:
        CHK(write_u32(b, (uint32_t)v->as.str.len));
        return write_bytes(b, v->as.str.s, v->as.str.len);
    case SC_VAL_BOOL: return write_u8(b, v->as.boolean ? 1 : 0);
    case SC_VAL_ERR:  return write_i32(b, (int32_t)v->as.err);
    case SC_VAL_EMPTY: return SC_OK;
    default: return SC_ERR_TYPE;
    }
}

static int fml_tab_find(const CsFmlTab *ft, const char *text, size_t tlen)
{
    int i;
    for (i = 0; i < ft->count; i++) {
        ScFormula *f = ft->items[i];
        if (f && f->text && f->text_len == tlen && memcmp(f->text, text, tlen) == 0)
            return i;
    }
    return -1;
}

static int fml_tab_add(CsFmlTab *ft, ScFormula *f)
{
    int idx;
    if (!f || !f->text) return SC_ERR_INTERNAL;
    idx = fml_tab_find(ft, f->text, f->text_len);
    if (idx >= 0) return idx;
    if (ft->count >= ft->cap) {
        int nc = ft->cap ? ft->cap * 2 : 16;
        ScFormula **na = (ScFormula **)realloc(ft->items, (size_t)nc * sizeof(ScFormula *));
        if (!na) return SC_ERR_NOMEM;
        ft->items = na;
        ft->cap = nc;
    }
    ft->items[ft->count] = f;
    return ft->count++;
}

static int str_tab_find(const CsStrTab *st, const char *s, size_t slen)
{
    uint32_t i;
    for (i = 0; i < st->count; i++) {
        size_t n = strlen(st->items[i]);
        if (n == slen && memcmp(st->items[i], s, slen) == 0)
            return (int)i;
    }
    return -1;
}

static int str_tab_add(CsStrTab *st, const char *s, size_t slen)
{
    char *copy;
    if (str_tab_find(st, s, slen) >= 0) return SC_OK;
    if (st->count >= st->cap) {
        uint32_t nc = st->cap ? st->cap * 2u : 32u;
        char **na = (char **)realloc(st->items, (size_t)nc * sizeof(char *));
        if (!na) return SC_ERR_NOMEM;
        st->items = na;
        st->cap = nc;
    }
    copy = (char *)malloc(slen + 1);
    if (!copy) return SC_ERR_NOMEM;
    if (slen) memcpy(copy, s, slen);
    copy[slen] = '\0';
    st->items[st->count++] = copy;
    return SC_OK;
}

static int collect_tables(const ScWorkbook *wb, CsFmlTab *ft, CsStrTab *st)
{
    int si, r, c;
    memset(ft, 0, sizeof(*ft));
    memset(st, 0, sizeof(*st));
    for (si = 0; si < wb->nsheets; si++) {
        ScSheet *sh = &wb->sheets[si];
        if (!sh->cells || sh->nrows <= 0 || sh->ncols <= 0) continue;
        for (r = 0; r < sh->nrows; r++) {
            for (c = 0; c < sh->ncols; c++) {
                ScCell *cl;
                int rc;
                if (sci_cell_at(sh, r, c, &cl) != SC_OK) continue;
                if (cl->formula && cl->formula->text) {
                    rc = fml_tab_add(ft, cl->formula);
                    if (rc < 0) return rc;
                }
                if (cl->value.type == SC_VAL_STR) {
                    rc = str_tab_add(st, cl->value.as.str.s, cl->value.as.str.len);
                    if (rc != SC_OK) return rc;
                }
            }
        }
    }
    return SC_OK;
}

static void str_tab_free(CsStrTab *st)
{
    uint32_t i;
    if (!st) return;
    for (i = 0; i < st->count; i++) free(st->items[i]);
    free(st->items);
    memset(st, 0, sizeof(*st));
}

static void fml_tab_free(CsFmlTab *ft)
{
    free(ft->items);
    memset(ft, 0, sizeof(*ft));
}

static int write_sheets_section(ScBuf *b, const ScWorkbook *wb)
{
    int i;
    if (wb->nsheets <= 0 || wb->nsheets > 65535) return SC_ERR_FORMAT;
    CHK(write_u16(b, (uint16_t)wb->nsheets));
    for (i = 0; i < wb->nsheets; i++) {
        ScSheet *sh = &wb->sheets[i];
        uint16_t nlen = (uint16_t)sh->name_len;
        if (!sh->name || nlen == 0) return SC_ERR_FORMAT;
        CHK(write_u16(b, nlen));
        CHK(write_bytes(b, sh->name, nlen));
        CHK(write_u32(b, (uint32_t)sh->nrows));
        CHK(write_u32(b, (uint32_t)sh->ncols));
        CHK(write_u32(b, sh->flags));
    }
    return SC_OK;
}

static int write_cells_section(ScBuf *b, const ScWorkbook *wb, const CsFmlTab *ft)
{
    int si, r, c;
    for (si = 0; si < wb->nsheets; si++) {
        ScSheet *sh = &wb->sheets[si];
        if (!sh->cells || sh->nrows <= 0 || sh->ncols <= 0) continue;
        for (r = 0; r < sh->nrows; r++) {
            for (c = 0; c < sh->ncols; c++) {
                ScCell *cl;
                int32_t fidx = -1;
                if (sci_cell_at(sh, r, c, &cl) != SC_OK) continue;
                if (!cell_has_data(cl)) continue;
                if (cl->formula && cl->formula->text) {
                    fidx = fml_tab_find(ft, cl->formula->text, cl->formula->text_len);
                    if (fidx < 0) return SC_ERR_INTERNAL;
                }
                CHK(write_u16(b, (uint16_t)si));
                CHK(write_u32(b, (uint32_t)r));
                CHK(write_u32(b, (uint32_t)c));
                CHK(write_cell_value(b, &cl->value));
                CHK(write_i32(b, fidx));
                CHK(write_u32(b, cl->style_id));
            }
        }
    }
    return SC_OK;
}

static int write_formulas_section(ScBuf *b, const CsFmlTab *ft)
{
    int i;
    CHK(write_u32(b, (uint32_t)ft->count));
    for (i = 0; i < ft->count; i++) {
        ScFormula *f = ft->items[i];
        uint32_t tlen;
        if (!f || !f->text) return SC_ERR_INTERNAL;
        tlen = (uint32_t)f->text_len;
        if (tlen == 0 || tlen > SC_MAX_FORMULA_LEN) return SC_ERR_FORMAT;
        CHK(write_u32(b, tlen));
        CHK(write_bytes(b, f->text, tlen));
    }
    return SC_OK;
}

static int write_strings_section(ScBuf *b, const CsStrTab *st)
{
    uint32_t i;
    CHK(write_u32(b, st->count));
    for (i = 0; i < st->count; i++) {
        size_t slen = strlen(st->items[i]);
        CHK(write_u32(b, (uint32_t)slen));
        CHK(write_bytes(b, st->items[i], slen));
    }
    return SC_OK;
}

static int write_styles_section(ScBuf *b, const ScWorkbook *wb)
{
    int i;
    CHK(write_u32(b, (uint32_t)wb->nstyles));
    for (i = 0; i < wb->nstyles; i++) {
        CsStyle *st = &wb->styles[i];
        CHK(write_u32(b, st->fmt_id));
        CHK(write_u32(b, st->font_id));
        CHK(write_u32(b, st->fill_color));
        CHK(write_u32(b, st->border_mask));
        CHK(write_u8(b, st->align));
        CHK(write_u8(b, st->wrap));
    }
    return SC_OK;
}

static int write_names_section(ScBuf *b, const ScWorkbook *wb)
{
    int i;
    if (wb->nnames > 65535) return SC_ERR_OVERFLOW;
    CHK(write_u16(b, (uint16_t)wb->nnames));
    for (i = 0; i < wb->nnames; i++) {
        CsNameDef *nd = &wb->names[i];
        size_t nlen = nd->name ? strlen(nd->name) : 0;
        if (nlen == 0 || nlen > SC_MAX_SHEET_NAME) return SC_ERR_FORMAT;
        CHK(write_u16(b, (uint16_t)nlen));
        CHK(write_bytes(b, nd->name, nlen));
        CHK(write_i32(b, (int32_t)nd->sheet));
        CHK(write_u32(b, (uint32_t)nd->r0));
        CHK(write_u32(b, (uint32_t)nd->c0));
        CHK(write_u32(b, (uint32_t)nd->r1));
        CHK(write_u32(b, (uint32_t)nd->c1));
    }
    return SC_OK;
}

typedef int (*SecWriterFn)(ScBuf *, const ScWorkbook *, const CsFmlTab *, const CsStrTab *);

static int write_sec_entry(ScBuf *b, CsSecSlot *slot, uint32_t type)
{
    slot->type = type;
    slot->off_pos = b->len + 4;
    CHK(write_u32(b, type));
    CHK(write_u32(b, 0));
    return write_u32(b, 0);
}

static int finish_section(ScBuf *b, CsSecSlot *slot, size_t sec_start)
{
    uint32_t off = (uint32_t)sec_start;
    uint32_t sz = (uint32_t)(b->len - sec_start);
    if ((size_t)off != sec_start || (size_t)sz != b->len - sec_start)
        return SC_ERR_OVERFLOW;
    CHK(patch_u32(b, slot->off_pos, off));
    return patch_u32(b, slot->off_pos + 4, sz);
}

static int emit_section(ScBuf *b, CsSecSlot *slot, const ScWorkbook *wb,
                        const CsFmlTab *ft, const CsStrTab *st,
                        SecWriterFn fn)
{
    size_t start = b->len;
    int rc = fn(b, wb, ft, st);
    if (rc != SC_OK) return rc;
    return finish_section(b, slot, start);
}

static int sec_sheets(ScBuf *b, const ScWorkbook *wb,
                      const CsFmlTab *ft, const CsStrTab *st)
{
    (void)ft; (void)st;
    return write_sheets_section(b, wb);
}

static int sec_cells(ScBuf *b, const ScWorkbook *wb,
                     const CsFmlTab *ft, const CsStrTab *st)
{
    (void)st;
    return write_cells_section(b, wb, ft);
}

static int sec_formulas(ScBuf *b, const ScWorkbook *wb,
                        const CsFmlTab *ft, const CsStrTab *st)
{
    (void)wb; (void)st;
    return write_formulas_section(b, ft);
}

static int sec_strings(ScBuf *b, const ScWorkbook *wb,
                       const CsFmlTab *ft, const CsStrTab *st)
{
    (void)wb; (void)ft;
    return write_strings_section(b, st);
}

static int sec_styles(ScBuf *b, const ScWorkbook *wb,
                      const CsFmlTab *ft, const CsStrTab *st)
{
    (void)ft; (void)st;
    return write_styles_section(b, wb);
}

static int sec_names(ScBuf *b, const ScWorkbook *wb,
                     const CsFmlTab *ft, const CsStrTab *st)
{
    (void)ft; (void)st;
    return write_names_section(b, wb);
}

int sc_workbook_write(const sc_workbook *wb, uint8_t **out, size_t *out_len)
{
    ScBuf buf;
    CsFmlTab fml_tab;
    CsStrTab str_tab;
    CsSecSlot slots[6];
    static const uint32_t sec_types[] = {
        SC_SEC_SHEETS, SC_SEC_CELLS, SC_SEC_FORMULAS,
        SC_SEC_STRINGS, SC_SEC_STYLES, SC_SEC_NAMES
    };
    static const SecWriterFn writers[] = {
        sec_sheets, sec_cells, sec_formulas,
        sec_strings, sec_styles, sec_names
    };
    const ScWorkbook *w;
    uint32_t ver, flags;
    int rc, i;
    uint8_t *raw;

    if (!wb || !out || !out_len) return SC_ERR_INTERNAL;
    *out = NULL;
    *out_len = 0;
    w = SC_WB(wb);
    if (w->nsheets <= 0) return SC_ERR_FORMAT;

    rc = collect_tables(w, &fml_tab, &str_tab);
    if (rc != SC_OK) goto done;

    sci_buf_init(&buf);
    CHK_G(write_bytes(&buf, "CSB\0", 4));
    ver = w->version ? w->version : 1u;
    flags = w->flags;
    CHK_G(write_u32(&buf, ver));
    CHK_G(write_u32(&buf, flags));
    CHK_G(write_u32(&buf, 6u));

    for (i = 0; i < 6; i++) {
        rc = write_sec_entry(&buf, &slots[i], sec_types[i]);
        if (rc != SC_OK) goto done;
    }
    for (i = 0; i < 6; i++) {
        rc = emit_section(&buf, &slots[i], w, &fml_tab, &str_tab, writers[i]);
        if (rc != SC_OK) goto done;
    }

    raw = (uint8_t *)malloc(buf.len);
    if (!raw) { rc = SC_ERR_NOMEM; goto done; }
    memcpy(raw, buf.data, buf.len);
    *out = raw;
    *out_len = buf.len;
    rc = SC_OK;

done:
    fml_tab_free(&fml_tab);
    str_tab_free(&str_tab);
    sci_buf_free(&buf);
    return rc;
}
