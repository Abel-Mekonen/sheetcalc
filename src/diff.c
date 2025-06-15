/*
 * diff.c — workbook diff engine
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

typedef enum {
    SC_DIFF_ADD = 1,    /* cell exists in B but not A */
    SC_DIFF_DEL,        /* cell exists in A but not B */
    SC_DIFF_MOD,        /* cell exists in both, value differs */
    SC_DIFF_FML,        /* formula text changed */
    SC_DIFF_STYLE,      /* style changed */
} CsDiffKind;

typedef struct {
    CsDiffKind kind;
    int sheet, row, col;
    ScVal old_val;  /* for MOD/DEL */
    ScVal new_val;  /* for MOD/ADD */
} CsDiffEntry;

typedef struct {
    CsDiffEntry *entries;
    size_t count, cap;
} CsDiffResult;

#define DIFF_INIT_CAP  64

static int cell_used(const ScCell *cl)
{
    if (!cl)
        return 0;
    if (cl->formula)
        return 1;
    return cl->value.type != SC_VAL_EMPTY && cl->value.type != SC_VAL_NIL;
}

static int get_cell(const ScSheet *sh, int row, int col, ScCell **out)
{
    if (!sh || !out)
        return SC_ERR_RANGE;
    if (row < 0 || col < 0 || row >= sh->nrows || col >= sh->ncols)
        return SC_ERR_RANGE;
    return sci_cell_at((ScSheet *)sh, row, col, out);
}

static int vals_equal(const ScVal *a, const ScVal *b)
{
    int ta, tb;

    if (!a || !b)
        return 0;
    ta = a->type;
    tb = b->type;
    if ((ta == SC_VAL_EMPTY || ta == SC_VAL_NIL) &&
        (tb == SC_VAL_EMPTY || tb == SC_VAL_NIL))
        return 1;
    if (ta != tb)
        return 0;
    return sc_val_cmp(a, b) == 0;
}

static int fml_equal(const ScCell *a, const ScCell *b)
{
    const char *ta, *tb;
    size_t la, lb;

    if (!a && !b)
        return 1;
    if (!a || !b)
        return !a->formula && !b->formula;
    if (!a->formula && !b->formula)
        return 1;
    if (!a->formula || !b->formula)
        return 0;
    ta = a->formula->text;
    tb = b->formula->text;
    if (!ta || !tb)
        return ta == tb;
    la = a->formula->text_len ? a->formula->text_len : strlen(ta);
    lb = b->formula->text_len ? b->formula->text_len : strlen(tb);
    if (la != lb)
        return 0;
    return memcmp(ta, tb, la) == 0;
}

static int add_entry(CsDiffResult *out, CsDiffKind kind,
                     int sheet, int row, int col,
                     const ScVal *old_v, const ScVal *new_v)
{
    CsDiffEntry *e;

    if (!out)
        return SC_ERR_RANGE;
    if (out->count >= out->cap) {
        size_t nc = out->cap ? out->cap * 2 : DIFF_INIT_CAP;
        CsDiffEntry *ne = (CsDiffEntry *)realloc(out->entries, nc * sizeof(CsDiffEntry));
        if (!ne)
            return SC_ERR_NOMEM;
        out->entries = ne;
        out->cap = nc;
    }
    e = &out->entries[out->count++];
    e->kind = kind;
    e->sheet = sheet;
    e->row = row;
    e->col = col;
    sc_val_nil(&e->old_val);
    sc_val_nil(&e->new_val);
    if (old_v && sc_val_copy(&e->old_val, old_v) != SC_OK)
        return SC_ERR_NOMEM;
    if (new_v && sc_val_copy(&e->new_val, new_v) != SC_OK)
        return SC_ERR_NOMEM;
    return SC_OK;
}

static int diff_sheet(const ScSheet *a, const ScSheet *b,
                      int sheet_idx, CsDiffResult *out)
{
    int max_r, max_c, r, c;
    int rc = SC_OK;

    max_r = 0;
    max_c = 0;
    if (a) {
        if (a->nrows > max_r) max_r = a->nrows;
        if (a->ncols > max_c) max_c = a->ncols;
    }
    if (b) {
        if (b->nrows > max_r) max_r = b->nrows;
        if (b->ncols > max_c) max_c = b->ncols;
    }

    for (r = 0; r < max_r && rc == SC_OK; r++) {
        for (c = 0; c < max_c && rc == SC_OK; c++) {
            ScCell *ca = NULL, *cb = NULL;
            int ua, ub;

            if (a)
                get_cell(a, r, c, &ca);
            if (b)
                get_cell(b, r, c, &cb);
            ua = cell_used(ca);
            ub = cell_used(cb);

            if (ua && !ub) {
                rc = add_entry(out, SC_DIFF_DEL, sheet_idx, r, c,
                               ca ? &ca->value : NULL, NULL);
                continue;
            }
            if (!ua && ub) {
                rc = add_entry(out, SC_DIFF_ADD, sheet_idx, r, c,
                               NULL, cb ? &cb->value : NULL);
                continue;
            }
            if (!ua && !ub)
                continue;

            if (!vals_equal(&ca->value, &cb->value)) {
                rc = add_entry(out, SC_DIFF_MOD, sheet_idx, r, c,
                               &ca->value, &cb->value);
                if (rc != SC_OK)
                    break;
            }
            if (!fml_equal(ca, cb)) {
                rc = add_entry(out, SC_DIFF_FML, sheet_idx, r, c,
                               &ca->value, &cb->value);
                if (rc != SC_OK)
                    break;
            }
            if (ca->style_id != cb->style_id) {
                rc = add_entry(out, SC_DIFF_STYLE, sheet_idx, r, c,
                               &ca->value, &cb->value);
            }
        }
    }
    return rc;
}

static int find_sheet_by_name(const ScWorkbook *wb, const char *name)
{
    int i;

    if (!wb || !name)
        return -1;
    for (i = 0; i < wb->nsheets; i++) {
        if (wb->sheets[i].name && strcmp(wb->sheets[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int ensure_cell(ScWorkbook *wb, int sh_idx, int row, int col, ScCell **out)
{
    ScSheet *sh;
    size_t need, cap, i;

    if (!wb || sh_idx < 0 || sh_idx >= wb->nsheets || row < 0 || col < 0)
        return SC_ERR_RANGE;
    sh = &wb->sheets[sh_idx];
    if (sh->ncols <= 0)
        sh->ncols = col + 1;
    if (row >= sh->nrows || col >= sh->ncols) {
        int nr = row + 1 > sh->nrows ? row + 1 : sh->nrows;
        int nc = col + 1 > sh->ncols ? col + 1 : sh->ncols;
        need = (size_t)nr * (size_t)nc;
        if (need > sh->cell_cap) {
            cap = sh->cell_cap ? sh->cell_cap : SC_BUF_INIT_CAP;
            while (cap < need)
                cap *= 2;
            ScCell *ncells = (ScCell *)realloc(sh->cells, cap * sizeof(ScCell));
            if (!ncells)
                return SC_ERR_NOMEM;
            for (i = sh->cell_cap; i < cap; i++) {
                sc_val_nil(&ncells[i].value);
                ncells[i].formula = NULL;
                ncells[i].dirty = 0;
                ncells[i].style_id = 0;
            }
            sh->cells = ncells;
            sh->cell_cap = cap;
        }
        sh->nrows = nr;
        sh->ncols = nc;
    }
    return sci_cell_at(sh, row, col, out);
}

static const char *kind_tag(CsDiffKind k)
{
    switch (k) {
    case SC_DIFF_ADD:   return "ADD";
    case SC_DIFF_DEL:   return "DEL";
    case SC_DIFF_MOD:   return "MOD";
    case SC_DIFF_FML:   return "FML";
    case SC_DIFF_STYLE: return "STYLE";
    default:            return "?";
    }
}

int cs_diff(const sc_workbook *a, const sc_workbook *b, CsDiffResult *out)
{
    const ScWorkbook *wa, *wb;
    int *matched;
    int si, bi, rc = SC_OK;

    if (!out)
        return SC_ERR_RANGE;
    out->entries = NULL;
    out->count = out->cap = 0;
    if (!a && !b)
        return SC_OK;

    wa = a ? SC_WB(a) : NULL;
    wb = b ? SC_WB(b) : NULL;

    if (wa && wa->nsheets > 0) {
        matched = (int *)calloc((size_t)wb ? (size_t)wb->nsheets : 1, sizeof(int));
        if (!matched && wb && wb->nsheets > 0)
            return SC_ERR_NOMEM;

        for (si = 0; si < wa->nsheets && rc == SC_OK; si++) {
            const ScSheet *sa = &wa->sheets[si];
            const ScSheet *sb = NULL;
            bi = wb ? find_sheet_by_name(wb, sa->name) : -1;
            if (bi >= 0 && wb) {
                sb = &wb->sheets[bi];
                matched[bi] = 1;
            }
            rc = diff_sheet(sa, sb, si, out);
        }

        if (wb && rc == SC_OK) {
            for (bi = 0; bi < wb->nsheets; bi++) {
                if (matched[bi])
                    continue;
                rc = diff_sheet(NULL, &wb->sheets[bi], bi, out);
                if (rc != SC_OK)
                    break;
            }
        }
        free(matched);
    } else if (wb) {
        for (bi = 0; bi < wb->nsheets && rc == SC_OK; bi++)
            rc = diff_sheet(NULL, &wb->sheets[bi], bi, out);
    }
    return rc;
}

void cs_diff_free(CsDiffResult *r)
{
    size_t i;

    if (!r)
        return;
    for (i = 0; i < r->count; i++) {
        sc_val_free(&r->entries[i].old_val);
        sc_val_free(&r->entries[i].new_val);
    }
    free(r->entries);
    r->entries = NULL;
    r->count = r->cap = 0;
}

int cs_diff_summary(const CsDiffResult *r, ScBuf *sb)
{
    size_t i;
    size_t counts[6];
    ScBuf tmp;
    int rc;

    if (!r || !sb)
        return SC_ERR_RANGE;

    memset(counts, 0, sizeof(counts));
    for (i = 0; i < r->count; i++) {
        int k = (int)r->entries[i].kind;
        if (k >= 1 && k <= 5)
            counts[k]++;
    }

    rc = sci_buf_appendf(sb,
        "diff: %zu change(s) — add=%zu del=%zu mod=%zu fml=%zu style=%zu\n",
        r->count, counts[SC_DIFF_ADD], counts[SC_DIFF_DEL], counts[SC_DIFF_MOD],
        counts[SC_DIFF_FML], counts[SC_DIFF_STYLE]);
    if (rc < 0)
        return SC_ERR_NOMEM;

    sci_buf_init(&tmp);
    for (i = 0; i < r->count; i++) {
        const CsDiffEntry *e = &r->entries[i];

        sci_buf_reset(&tmp);
        rc = sci_buf_appendf(sb, "  [%s] sh%d r%d c%d",
                             kind_tag(e->kind), e->sheet, e->row, e->col);
        if (rc < 0) { rc = SC_ERR_NOMEM; goto done; }

        if (e->kind == SC_DIFF_DEL || e->kind == SC_DIFF_MOD ||
            e->kind == SC_DIFF_FML || e->kind == SC_DIFF_STYLE) {
            rc = sc_val_to_str(&e->old_val, &tmp);
            if (rc != SC_OK)
                goto done;
            rc = sci_buf_appendf(sb, "  old=%s", sci_buf_cstr(&tmp));
            if (rc < 0) { rc = SC_ERR_NOMEM; goto done; }
        }
        if (e->kind == SC_DIFF_ADD || e->kind == SC_DIFF_MOD ||
            e->kind == SC_DIFF_FML || e->kind == SC_DIFF_STYLE) {
            sci_buf_reset(&tmp);
            rc = sc_val_to_str(&e->new_val, &tmp);
            if (rc != SC_OK)
                goto done;
            rc = sci_buf_appendf(sb, "  new=%s", sci_buf_cstr(&tmp));
            if (rc < 0) { rc = SC_ERR_NOMEM; goto done; }
        }
        if (sci_buf_append(sb, "\n", 1) < 0) {
            rc = SC_ERR_NOMEM;
            goto done;
        }
    }
    rc = SC_OK;

done:
    sci_buf_free(&tmp);
    return rc;
}

int cs_diff_apply(sc_workbook *target, const CsDiffResult *diff)
{
    ScWorkbook *wb;
    size_t i;
    int rc = SC_OK;

    if (!target || !diff)
        return SC_ERR_RANGE;
    wb = SC_WB(target);

    for (i = 0; i < diff->count && rc == SC_OK; i++) {
        const CsDiffEntry *e = &diff->entries[i];
        ScCell *cl;

        if (e->sheet < 0 || e->sheet >= wb->nsheets)
            continue;
        if (e->kind == SC_DIFF_FML || e->kind == SC_DIFF_STYLE)
            continue; /* no fml/style payload in entry */

        if (e->kind == SC_DIFF_DEL) {
            if (get_cell(&wb->sheets[e->sheet], e->row, e->col, &cl) != SC_OK)
                continue;
            sc_val_free(&cl->value);
            sc_val_nil(&cl->value);
            if (cl->formula) {
                free(cl->formula->text);
                if (cl->formula->root)
                    ; /* root owned by formula; leak avoided below */
                free(cl->formula);
                cl->formula = NULL;
            }
            cl->style_id = 0;
            cl->dirty = 0;
            continue;
        }

        rc = ensure_cell(wb, e->sheet, e->row, e->col, &cl);
        if (rc != SC_OK)
            break;
        sc_val_free(&cl->value);
        if (sc_val_copy(&cl->value, &e->new_val) != SC_OK) {
            rc = SC_ERR_NOMEM;
            break;
        }
        cl->dirty = 1;
    }
    if (rc == SC_OK)
        wb->flags |= SC_WB_DIRTY;
    return rc;
}

void cs_diff_filter(CsDiffResult *r, CsDiffKind kind)
{
    size_t rd, wr;

    if (!r || !r->entries)
        return;
    for (rd = 0, wr = 0; rd < r->count; rd++) {
        CsDiffEntry *e = &r->entries[rd];
        if (e->kind != kind) {
            sc_val_free(&e->old_val);
            sc_val_free(&e->new_val);
            continue;
        }
        if (wr != rd)
            r->entries[wr] = *e;
        wr++;
    }
    r->count = wr;
}
