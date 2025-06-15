/*
 * parser.c — .scb (CellScript Binary) format decoder
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

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} CsReader;

static int rd_check(CsReader *rd, size_t need)
{
    if (!rd || !rd->data || need > rd->len || rd->pos > rd->len - need)
        return SC_ERR_CORRUPT;
    return SC_OK;
}

static int rd_u8(CsReader *rd, uint8_t *out)
{
    int rc = rd_check(rd, 1);
    if (rc != SC_OK) return rc;
    *out = rd->data[rd->pos++];
    return SC_OK;
}

static int rd_u16(CsReader *rd, uint16_t *out)
{
    int rc = rd_check(rd, 2);
    if (rc != SC_OK) return rc;
    *out = (uint16_t)(rd->data[rd->pos] | (rd->data[rd->pos + 1] << 8));
    rd->pos += 2;
    return SC_OK;
}

static int rd_u32(CsReader *rd, uint32_t *out)
{
    int rc = rd_check(rd, 4);
    if (rc != SC_OK) return rc;
    *out = (uint32_t)rd->data[rd->pos]
         | ((uint32_t)rd->data[rd->pos + 1] << 8)
         | ((uint32_t)rd->data[rd->pos + 2] << 16)
         | ((uint32_t)rd->data[rd->pos + 3] << 24);
    rd->pos += 4;
    return SC_OK;
}

static int rd_i32(CsReader *rd, int32_t *out)
{
    uint32_t u;
    int rc = rd_u32(rd, &u);
    if (rc == SC_OK) *out = (int32_t)u;
    return rc;
}

static int rd_f64(CsReader *rd, double *out)
{
    union { uint64_t u; double d; } v;
    uint32_t lo, hi;
    int rc = rd_u32(rd, &lo);
    if (rc != SC_OK) return rc;
    rc = rd_u32(rd, &hi);
    if (rc != SC_OK) return rc;
    v.u = (uint64_t)lo | ((uint64_t)hi << 32);
    *out = v.d;
    return SC_OK;
}

static int rd_bytes(CsReader *rd, size_t n, const uint8_t **out)
{
    int rc = rd_check(rd, n);
    if (rc != SC_OK) return rc;
    *out = rd->data + rd->pos;
    rd->pos += n;
    return SC_OK;
}

static int rd_skip(CsReader *rd, size_t n)
{
    int rc = rd_check(rd, n);
    if (rc == SC_OK) rd->pos += n;
    return rc;
}

static int rd_str_copy(CsReader *rd, size_t n, char **out)
{
    const uint8_t *p;
    char *s;
    int rc = rd_bytes(rd, n, &p);
    if (rc != SC_OK) return rc;
    s = (char *)malloc(n + 1);
    if (!s) return SC_ERR_NOMEM;
    if (n) memcpy(s, p, n);
    s[n] = '\0';
    *out = s;
    return SC_OK;
}

static CsReader rd_slice(const CsReader *base, uint32_t off, uint32_t sz)
{
    CsReader s = { base->data, base->len, (size_t)off };
    if ((uint64_t)off + (uint64_t)sz > base->len)
        s.len = 0;
    else
        s.len = (size_t)off + (size_t)sz;
    return s;
}

static int check_remaining(CsReader *rd, size_t *out)
{
    if (!rd || rd->pos > rd->len) return SC_ERR_CORRUPT;
    *out = rd->len - rd->pos;
    return SC_OK;
}

#define CSB_HDR_SZ      16u
#define CSB_SEC_ENT_SZ  12u
#define CSB_MAX_SECS    32u   /* wire format has 7 types; headroom for fuzz */

enum {
    SC_SEC_SHEETS = 1, SC_SEC_CELLS, SC_SEC_FORMULAS, SC_SEC_STRINGS,
    SC_SEC_STYLES, SC_SEC_NAMES, SC_SEC_METADATA
};

typedef struct { uint32_t type, off, sz; int seen; } CsSecEnt;

typedef struct { ScFormula **items; int count; } CsFmlTab;
typedef struct { char **items; uint32_t count; } CsStrTab;

static void fml_tab_free(CsFmlTab *ft)
{
    int i;
    if (!ft) return;
    for (i = 0; i < ft->count; i++) {
        if (ft->items[i]) {
            free(ft->items[i]->text);
            free(ft->items[i]);
        }
    }
    free(ft->items);
    ft->items = NULL;
    ft->count = 0;
}

static void str_tab_free(CsStrTab *st)
{
    uint32_t i;
    if (!st) return;
    for (i = 0; i < st->count; i++) free(st->items[i]);
    free(st->items);
    st->items = NULL;
    st->count = 0;
}

static void wb_destroy(ScWorkbook *wb)
{
    int si, i;
    if (!wb) return;
    for (si = 0; si < wb->nsheets; si++) {
        ScSheet *sh = &wb->sheets[si];
        free(sh->name);
        if (sh->cells) {
            size_t n = (size_t)sh->nrows * (size_t)sh->ncols;
            for (i = 0; (size_t)i < n; i++) {
                sc_val_free(&sh->cells[i].value);
                if (sh->cells[i].formula) {
                    free(sh->cells[i].formula->text);
                    free(sh->cells[i].formula);
                }
            }
            free(sh->cells);
        }
    }
    for (i = 0; i < wb->nnames; i++) free(wb->names[i].name);
    free(wb->sheets);
    free(wb->names);
    free(wb->styles);
    sci_buf_free(&wb->eval.sb);
    free(wb->raw);
    free(wb);
}

static int wb_init(ScWorkbook **out)
{
    ScWorkbook *wb = (ScWorkbook *)calloc(1, sizeof(*wb));
    if (!wb) return SC_ERR_NOMEM;
    sci_buf_init(&wb->eval.sb);
    wb->eval.wb = wb;
    *out = wb;
    return SC_OK;
}

static int parse_hdr(CsReader *rd, uint32_t *ver, uint32_t *flags, uint32_t *sec_cnt)
{
    if (rd_check(rd, CSB_HDR_SZ) != SC_OK)
        return SC_ERR_FORMAT;
    if (rd->data[0] != 'C' || rd->data[1] != 'S' ||
        rd->data[2] != 'B' || rd->data[3] != '\0')
        return SC_ERR_FORMAT;
    rd->pos = 4;
    if (rd_u32(rd, ver) != SC_OK || rd_u32(rd, flags) != SC_OK ||
        rd_u32(rd, sec_cnt) != SC_OK)
        return SC_ERR_FORMAT;
    if (*ver == 0 || *ver > 1) return SC_ERR_FORMAT; /* 0x1 = current */
    if (*sec_cnt > CSB_MAX_SECS) return SC_ERR_OVERFLOW;
    return SC_OK;
}

static int parse_sec_table(CsReader *rd, uint32_t sec_cnt, CsSecEnt *tbl)
{
    uint32_t i;
    memset(tbl, 0, sizeof(CsSecEnt) * CSB_MAX_SECS);
    for (i = 0; i < sec_cnt; i++) {
        uint32_t ty, off, sz;
        CsSecEnt *e;
        if (rd_u32(rd, &ty) != SC_OK || rd_u32(rd, &off) != SC_OK ||
            rd_u32(rd, &sz) != SC_OK)
            return SC_ERR_CORRUPT;
        if (ty < SC_SEC_SHEETS || ty > SC_SEC_METADATA)
            return SC_ERR_CORRUPT;
        e = &tbl[ty];
        if (e->seen) return SC_ERR_CORRUPT;
        e->type = ty; e->off = off; e->sz = sz; e->seen = 1;
        if ((uint64_t)off + (uint64_t)sz > (uint64_t)rd->len)
            return SC_ERR_CORRUPT;
    }
    return SC_OK;
}

static int parse_sec_sheets(CsReader *rd, ScWorkbook *wb)
{
    uint16_t cnt;
    int i, rc = rd_u16(rd, &cnt);
    if (rc != SC_OK || cnt == 0) return rc;
    wb->sheets = (ScSheet *)calloc(cnt, sizeof(ScSheet));
    if (!wb->sheets) return SC_ERR_NOMEM;
    wb->nsheets = (int)cnt;
    wb->sheet_cap = cnt;
    for (i = 0; i < wb->nsheets; i++) {
        ScSheet *sh = &wb->sheets[i];
        uint16_t nlen;
        uint32_t nr, nc, flg;
        size_t cellsz;
        rc = rd_u16(rd, &nlen);
        if (rc != SC_OK) return rc;
        if (nlen == 0 || nlen > SC_MAX_SHEET_NAME) return SC_ERR_CORRUPT;
        rc = rd_str_copy(rd, nlen, &sh->name);
        if (rc != SC_OK) return rc;
        sh->name_len = nlen;
        if (rd_u32(rd, &nr) != SC_OK || rd_u32(rd, &nc) != SC_OK ||
            rd_u32(rd, &flg) != SC_OK)
            return SC_ERR_CORRUPT;
        if (nr > INT32_MAX / 4 || nc > INT32_MAX / 4) return SC_ERR_OVERFLOW;
        sh->nrows = (int)nr; sh->ncols = (int)nc; sh->flags = flg;
        if (nr == 0 || nc == 0) continue;
        cellsz = (size_t)nr * (size_t)nc;
        if (cellsz > SIZE_MAX / sizeof(ScCell)) return SC_ERR_OVERFLOW;
        sh->cells = (ScCell *)calloc(cellsz, sizeof(ScCell));
        if (!sh->cells) return SC_ERR_NOMEM;
        sh->cell_cap = cellsz;
        for (size_t ci = 0; ci < cellsz; ci++)
            sc_val_nil(&sh->cells[ci].value);
    }
    return SC_OK;
}

static int parse_sec_formulas(CsReader *rd, CsFmlTab *ft)
{
    uint32_t cnt, i;
    int rc = rd_u32(rd, &cnt);
    if (rc != SC_OK || cnt == 0) { ft->count = 0; return rc; }
    if (cnt > 65536u) return SC_ERR_OVERFLOW;
    ft->items = (ScFormula **)calloc(cnt, sizeof(ScFormula *));
    if (!ft->items) return SC_ERR_NOMEM;
    ft->count = (int)cnt;
    for (i = 0; i < ft->count; i++) {
        ScFormula *f;
        uint32_t tlen;
        char *txt;
        rc = rd_u32(rd, &tlen);
        if (rc != SC_OK) return rc;
        if (tlen == 0 || tlen > SC_MAX_FORMULA_LEN) return SC_ERR_CORRUPT;
        rc = rd_str_copy(rd, tlen, &txt);
        if (rc != SC_OK) return rc;
        f = (ScFormula *)calloc(1, sizeof(*f));
        if (!f) { free(txt); return SC_ERR_NOMEM; }
        f->text = txt; f->text_len = tlen; f->root = NULL;
        ft->items[i] = f;
    }
    return SC_OK;
}

static int parse_sec_strings(CsReader *rd, CsStrTab *st)
{
    uint32_t cnt, i;
    int rc = rd_u32(rd, &cnt);
    if (rc != SC_OK) return rc;
    st->count = cnt;
    if (cnt == 0) { st->items = NULL; return SC_OK; }
    st->items = (char **)calloc(cnt, sizeof(char *));
    if (!st->items) return SC_ERR_NOMEM;
    for (i = 0; i < cnt; i++) {
        uint32_t slen;
        rc = rd_u32(rd, &slen);
        if (rc != SC_OK) return rc;
        if (slen > 1048576u) return SC_ERR_CORRUPT; /* 1 MiB cap */
        rc = rd_str_copy(rd, slen, &st->items[i]);
        if (rc != SC_OK) return rc;
    }
    return SC_OK;
}

static int parse_sec_styles(CsReader *rd, ScWorkbook *wb)
{
    uint32_t cnt, i;
    int rc = rd_u32(rd, &cnt);
    if (rc != SC_OK || cnt == 0) return rc;
    wb->styles = (CsStyle *)calloc(cnt, sizeof(CsStyle));
    if (!wb->styles) return SC_ERR_NOMEM;
    wb->nstyles = (int)cnt;
    wb->style_cap = cnt;
    for (i = 0; i < wb->nstyles; i++) {
        CsStyle *st = &wb->styles[i];
        uint32_t u32;
        uint8_t u8;
        if (rd_u32(rd, &u32) != SC_OK) return SC_ERR_CORRUPT;
        st->fmt_id = u32;
        if (rd_u32(rd, &u32) != SC_OK) return SC_ERR_CORRUPT;
        st->font_id = u32;
        if (rd_u32(rd, &u32) != SC_OK) return SC_ERR_CORRUPT;
        st->fill_color = u32;
        if (rd_u32(rd, &u32) != SC_OK) return SC_ERR_CORRUPT;
        st->border_mask = u32;
        if (rd_u8(rd, &u8) != SC_OK) return SC_ERR_CORRUPT;
        st->align = u8;
        if (rd_u8(rd, &u8) != SC_OK) return SC_ERR_CORRUPT;
        st->wrap = u8;
    }
    return SC_OK;
}

static int parse_sec_names(CsReader *rd, ScWorkbook *wb)
{
    uint16_t cnt;
    int i, rc = rd_u16(rd, &cnt);
    if (rc != SC_OK || cnt == 0) return rc;
    wb->names = (CsNameDef *)calloc(cnt, sizeof(CsNameDef));
    if (!wb->names) return SC_ERR_NOMEM;
    wb->nnames = (int)cnt;
    wb->name_cap = cnt;
    for (i = 0; i < wb->nnames; i++) {
        CsNameDef *nd = &wb->names[i];
        uint16_t nlen;
        int32_t sh;
        uint32_t r0, c0, r1, c1;
        rc = rd_u16(rd, &nlen);
        if (rc != SC_OK) return rc;
        if (nlen == 0 || nlen > SC_MAX_SHEET_NAME) return SC_ERR_CORRUPT;
        rc = rd_str_copy(rd, nlen, &nd->name);
        if (rc != SC_OK) return rc;
        if (rd_i32(rd, &sh) != SC_OK || rd_u32(rd, &r0) != SC_OK ||
            rd_u32(rd, &c0) != SC_OK || rd_u32(rd, &r1) != SC_OK ||
            rd_u32(rd, &c1) != SC_OK)
            return SC_ERR_CORRUPT;
        nd->sheet = sh; nd->r0 = (int)r0; nd->c0 = (int)c0;
        nd->r1 = (int)r1; nd->c1 = (int)c1;
        if (sh >= 0 && sh >= wb->nsheets) return SC_ERR_BADREF;
    }
    return SC_OK;
}

static int parse_sec_metadata(CsReader *rd)
{
    uint16_t cnt;
    int i, rc = rd_u16(rd, &cnt);
    if (rc != SC_OK) return rc;
    for (i = 0; i < cnt; i++) {
        uint16_t klen;
        uint32_t vlen;
        char *key;
        rc = rd_u16(rd, &klen);
        if (rc != SC_OK) return rc;
        if (klen == 0 || klen > 4096) return SC_ERR_CORRUPT;
        rc = rd_str_copy(rd, klen, &key);
        if (rc != SC_OK) return rc;
        rc = rd_u32(rd, &vlen);
        if (rc != SC_OK) { free(key); return rc; }
        if (vlen > 1048576u) { free(key); return SC_ERR_CORRUPT; }
        rc = rd_skip(rd, vlen);
        free(key);
        if (rc != SC_OK) return rc;
    }
    return SC_OK;
}

static int parse_cell_value(CsReader *rd, ScVal *v)
{
    uint8_t ty;
    int rc = rd_u8(rd, &ty);
    if (rc != SC_OK) return rc;
    switch (ty) {
    case SC_VAL_NUM: {
        double n;
        rc = rd_f64(rd, &n);
        if (rc != SC_OK) return rc;
        sc_val_num(v, n);
        break;
    }
    case SC_VAL_STR: {
        uint32_t slen;
        const uint8_t *p;
        rc = rd_u32(rd, &slen);
        if (rc != SC_OK) return rc;
        if (slen > 1048576u) return SC_ERR_CORRUPT;
        rc = rd_bytes(rd, slen, &p);
        if (rc != SC_OK) return rc;
        sc_val_str_n(v, (const char *)p, slen);
        break;
    }
    case SC_VAL_BOOL: {
        uint8_t b;
        rc = rd_u8(rd, &b);
        if (rc != SC_OK) return rc;
        sc_val_bool(v, b ? 1 : 0);
        break;
    }
    case SC_VAL_ERR: {
        int32_t ec;
        rc = rd_i32(rd, &ec);
        if (rc != SC_OK) return rc;
        sc_val_err(v, ec);
        break;
    }
    case SC_VAL_EMPTY:
        sc_val_nil(v);
        break;
    default:
        return SC_ERR_CORRUPT;
    }
    return SC_OK;
}

static int parse_sec_cells(CsReader *rd, ScWorkbook *wb, CsFmlTab *ft)
{
    size_t rem;
    int rc;
    for (;;) {
        uint16_t sh_idx;
        uint32_t row, col;
        int32_t fml_idx;
        uint32_t sty_id;
        ScSheet *sh;
        ScCell *cl;
        rc = check_remaining(rd, &rem);
        if (rc != SC_OK) return rc;
        if (rem == 0) break;
        if (rd_u16(rd, &sh_idx) != SC_OK || rd_u32(rd, &row) != SC_OK ||
            rd_u32(rd, &col) != SC_OK)
            return SC_ERR_CORRUPT;
        if ((int)sh_idx >= wb->nsheets) return SC_ERR_BADREF;
        sh = &wb->sheets[sh_idx];
        if ((int)row >= sh->nrows || (int)col >= sh->ncols) return SC_ERR_RANGE;
        rc = sci_cell_at(sh, (int)row, (int)col, &cl);
        if (rc != SC_OK) return rc;
        sc_val_free(&cl->value);
        rc = parse_cell_value(rd, &cl->value);
        if (rc != SC_OK) return rc;
        if (rd_i32(rd, &fml_idx) != SC_OK || rd_u32(rd, &sty_id) != SC_OK)
            return SC_ERR_CORRUPT;
        cl->style_id = sty_id;
        cl->dirty = 0;
        if (fml_idx >= 0) {
            if (fml_idx >= ft->count) return SC_ERR_BADREF;
            cl->formula = ft->items[fml_idx];
            ft->items[fml_idx] = NULL; /* cell owns it now */
        }
    }
    return SC_OK;
}

static int compile_formulas(ScWorkbook *wb)
{
    int si;
    for (si = 0; si < wb->nsheets; si++) {
        ScSheet *sh = &wb->sheets[si];
        size_t n, i;
        if (!sh->cells || sh->nrows <= 0 || sh->ncols <= 0) continue;
        n = (size_t)sh->nrows * (size_t)sh->ncols;
        for (i = 0; i < n; i++) {
            ScCell *cl = &sh->cells[i];
            ScFormula *raw, *compiled;
            int rc;
            if (!cl->formula || !cl->formula->text || cl->formula->root)
                continue;
            raw = cl->formula;
            rc = sci_parse_formula_text(wb, si, raw->text, raw->text_len, &compiled);
            if (rc != SC_OK) return rc;
            free(raw->text);
            free(raw);
            cl->formula = compiled;
        }
    }
    return SC_OK;
}

static int parse_one_sec(CsReader *base, const CsSecEnt *ent,
                         ScWorkbook *wb, CsFmlTab *ft, CsStrTab *st)
{
    CsReader rd;
    size_t rem;
    int rc;
    if (!ent->seen || ent->sz == 0) return SC_OK;
    rd = rd_slice(base, ent->off, ent->sz);
    if (rd.len == 0) return SC_ERR_CORRUPT;
    switch (ent->type) {
    case SC_SEC_SHEETS:   rc = parse_sec_sheets(&rd, wb); break;
    case SC_SEC_FORMULAS: rc = parse_sec_formulas(&rd, ft); break;
    case SC_SEC_STRINGS:  rc = parse_sec_strings(&rd, st); break;
    case SC_SEC_STYLES:   rc = parse_sec_styles(&rd, wb); break;
    case SC_SEC_NAMES:    rc = parse_sec_names(&rd, wb); break;
    case SC_SEC_METADATA: rc = parse_sec_metadata(&rd); break;
    case SC_SEC_CELLS:    rc = parse_sec_cells(&rd, wb, ft); break;
    default: rc = SC_ERR_CORRUPT; break;
    }
    if (rc != SC_OK) return rc;
    if (check_remaining(&rd, &rem) != SC_OK || rem != 0) return SC_ERR_CORRUPT;
    return SC_OK;
}

int sci_parse_workbook(const uint8_t *data, size_t len, ScWorkbook **out)
{
    CsReader rd;
    ScWorkbook *wb = NULL;
    CsFmlTab fml_tab;
    CsStrTab str_tab;
    CsSecEnt sec_tbl[CSB_MAX_SECS];
    uint32_t ver, flags, sec_cnt;
    size_t tbl_need;
    int rc, ty;
    uint8_t *raw_copy = NULL;
    static const int parse_order[] = {
        SC_SEC_SHEETS, SC_SEC_FORMULAS, SC_SEC_STRINGS,
        SC_SEC_STYLES, SC_SEC_NAMES, SC_SEC_CELLS, SC_SEC_METADATA
    };

    if (!data || !out) return SC_ERR_INTERNAL;
    *out = NULL;
    memset(&fml_tab, 0, sizeof(fml_tab));
    memset(&str_tab, 0, sizeof(str_tab));
    rd.data = data; rd.len = len; rd.pos = 0;

    rc = parse_hdr(&rd, &ver, &flags, &sec_cnt);
    if (rc != SC_OK) goto fail;

    tbl_need = (size_t)sec_cnt * CSB_SEC_ENT_SZ;
    if (tbl_need > len - CSB_HDR_SZ) { rc = SC_ERR_FORMAT; goto fail; }

    rc = parse_sec_table(&rd, sec_cnt, sec_tbl);
    if (rc != SC_OK) goto fail;

    rc = wb_init(&wb);
    if (rc != SC_OK) goto fail;
    wb->version = ver;
    wb->flags = flags;

    raw_copy = (uint8_t *)malloc(len);
    if (!raw_copy) { rc = SC_ERR_NOMEM; goto fail; }
    memcpy(raw_copy, data, len);
    wb->raw = raw_copy;
    wb->raw_len = len;

    for (size_t oi = 0; oi < sizeof(parse_order)/sizeof(parse_order[0]); oi++) {
        ty = parse_order[oi];
        if (!sec_tbl[ty].seen) continue;
        rc = parse_one_sec(&rd, &sec_tbl[ty], wb, &fml_tab, &str_tab);
        if (rc != SC_OK) goto fail;
    }

    /* orphan formula blobs — discard */
    for (int fi = 0; fi < fml_tab.count; fi++) {
        if (fml_tab.items[fi]) {
            free(fml_tab.items[fi]->text);
            free(fml_tab.items[fi]);
        }
    }

    rc = compile_formulas(wb);
    if (rc != SC_OK) goto fail;

    fml_tab_free(&fml_tab);
    str_tab_free(&str_tab);
    *out = wb;
    return SC_OK;

fail:
    fml_tab_free(&fml_tab);
    str_tab_free(&str_tab);
    wb_destroy(wb);
    return rc;
}
