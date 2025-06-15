/*
 * merge.c — workbook merge engine
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
    SC_MERGE_OURS = 0,  /* keep our value on conflict */
    SC_MERGE_THEIRS,    /* keep their value on conflict */
    SC_MERGE_NEWER,     /* keep whichever was modified more recently (by style_id as proxy) */
} CsMergeStrategy;

typedef struct {
    int sheet, row, col;
    ScVal ours, theirs;
} CsConflict;

typedef struct {
    CsConflict *conflicts;
    size_t count, cap;
} CsConflictList;

#define MERGE_INIT_CAP  32

static int cell_used(const ScCell *cl)
{
    if (!cl)
        return 0;
    if (cl->formula)
        return 1;
    return cl->value.type != SC_VAL_EMPTY && cl->value.type != SC_VAL_NIL;
}

static int vals_differ(const ScVal *a, const ScVal *b)
{
    int ta, tb;

    if (!a && !b)
        return 0;
    if (!a || !b)
        return 1;
    ta = a->type;
    tb = b->type;
    if ((ta == SC_VAL_EMPTY || ta == SC_VAL_NIL) &&
        (tb == SC_VAL_EMPTY || tb == SC_VAL_NIL))
        return 0;
    if (ta != tb)
        return 1;
    return sc_val_cmp(a, b) != 0;
}

static ScSheet *find_matching_sheet(ScWorkbook *wb, const char *name)
{
    int i;

    if (!wb || !name)
        return NULL;
    for (i = 0; i < wb->nsheets; i++) {
        if (wb->sheets[i].name && strcmp(wb->sheets[i].name, name) == 0)
            return &wb->sheets[i];
    }
    return NULL;
}

static int add_conflict(CsConflictList *cl, int sheet, int row, int col,
                        const ScVal *ours, const ScVal *theirs)
{
    CsConflict *c;

    if (!cl)
        return SC_ERR_RANGE;
    if (cl->count >= cl->cap) {
        size_t nc = cl->cap ? cl->cap * 2 : MERGE_INIT_CAP;
        CsConflict *ne = (CsConflict *)realloc(cl->conflicts, nc * sizeof(CsConflict));
        if (!ne)
            return SC_ERR_NOMEM;
        cl->conflicts = ne;
        cl->cap = nc;
    }
    c = &cl->conflicts[cl->count++];
    c->sheet = sheet;
    c->row = row;
    c->col = col;
    sc_val_nil(&c->ours);
    sc_val_nil(&c->theirs);
    if (ours && sc_val_copy(&c->ours, ours) != SC_OK)
        return SC_ERR_NOMEM;
    if (theirs && sc_val_copy(&c->theirs, theirs) != SC_OK)
        return SC_ERR_NOMEM;
    return SC_OK;
}

static int ensure_dims(ScSheet *sh, int row, int col)
{
    size_t need, cap, i;

    if (!sh || row < 0 || col < 0)
        return SC_ERR_RANGE;
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
    return SC_OK;
}

static int copy_formula(ScWorkbook *wb, int sh_idx, ScCell *dst, const ScCell *src)
{
    ScFormula *raw, *compiled;
    const char *txt;
    size_t tlen;
    int rc;

    if (!src || !src->formula || !src->formula->text)
        return SC_OK;
    txt = src->formula->text;
    tlen = src->formula->text_len ? src->formula->text_len : strlen(txt);
    raw = (ScFormula *)calloc(1, sizeof(ScFormula));
    if (!raw)
        return SC_ERR_NOMEM;
    raw->text = (char *)malloc(tlen + 1);
    if (!raw->text) {
        free(raw);
        return SC_ERR_NOMEM;
    }
    memcpy(raw->text, txt, tlen);
    raw->text[tlen] = '\0';
    raw->text_len = tlen;
    rc = sci_parse_formula_text(wb, sh_idx, raw->text, tlen, &compiled);
    free(raw->text);
    free(raw);
    if (rc != SC_OK)
        return rc;
    if (dst->formula) {
        free(dst->formula->text);
        free(dst->formula);
    }
    dst->formula = compiled;
    dst->dirty = 1;
    return SC_OK;
}

static int apply_theirs(ScWorkbook *wb, int sh_idx, ScCell *dst, const ScCell *src)
{
    int rc;

    sc_val_free(&dst->value);
    rc = sc_val_copy(&dst->value, &src->value);
    if (rc != SC_OK)
        return rc;
    dst->style_id = src->style_id;
    return copy_formula(wb, sh_idx, dst, src);
}

static int merge_cells(ScWorkbook *wb, ScSheet *base, const ScSheet *other,
                       int sh_idx, CsMergeStrategy strategy, CsConflictList *cl)
{
    int max_r, max_c, r, c;
    int rc = SC_OK;

    if (!wb || !base || !other)
        return SC_ERR_RANGE;

    max_r = base->nrows > other->nrows ? base->nrows : other->nrows;
    max_c = base->ncols > other->ncols ? base->ncols : other->ncols;

    for (r = 0; r < max_r && rc == SC_OK; r++) {
        for (c = 0; c < max_c && rc == SC_OK; c++) {
            ScCell *bc = NULL, *oc = NULL;
            int ub, uo;

            if (r < base->nrows && c < base->ncols)
                sci_cell_at(base, r, c, &bc);
            if (r < other->nrows && c < other->ncols)
                sci_cell_at((ScSheet *)other, r, c, &oc);
            ub = cell_used(bc);
            uo = cell_used(oc);

            if (!uo)
                continue;
            if (!ub) {
                rc = ensure_dims(base, r, c);
                if (rc != SC_OK)
                    break;
                rc = sci_cell_at(base, r, c, &bc);
                if (rc != SC_OK)
                    break;
                rc = apply_theirs(wb, sh_idx, bc, oc);
                continue;
            }
            if (!vals_differ(&bc->value, &oc->value) &&
                bc->style_id == oc->style_id)
                continue;

            if (cl) {
                rc = add_conflict(cl, sh_idx, r, c, &bc->value, &oc->value);
                if (rc != SC_OK)
                    break;
            }
            if (strategy == SC_MERGE_THEIRS ||
                (strategy == SC_MERGE_NEWER && oc->style_id > bc->style_id))
                rc = apply_theirs(wb, sh_idx, bc, oc);
        }
    }
    return rc;
}

int sc_merge_sheets(sc_workbook *base, const sc_workbook *other)
{
    ScWorkbook *b, *o;
    int oi, rc = SC_OK;

    if (!base || !other)
        return SC_ERR_RANGE;
    b = SC_WB(base);
    o = SC_WB(other);

    for (oi = 0; oi < o->nsheets && rc == SC_OK; oi++) {
        ScSheet *src = &o->sheets[oi];
        ScSheet *dst;
        size_t cellsz;

        if (find_matching_sheet(b, src->name))
            continue;
        if (b->nsheets >= (int)b->sheet_cap) {
            size_t cap = b->sheet_cap ? b->sheet_cap * 2 : 4;
            ScSheet *ns = (ScSheet *)realloc(b->sheets, cap * sizeof(ScSheet));
            if (!ns)
                return SC_ERR_NOMEM;
            memset(ns + b->nsheets, 0, (cap - (size_t)b->nsheets) * sizeof(ScSheet));
            b->sheets = ns;
            b->sheet_cap = cap;
        }
        dst = &b->sheets[b->nsheets++];
        memset(dst, 0, sizeof(*dst));
        if (src->name) {
            dst->name = (char *)malloc(src->name_len + 1);
            if (!dst->name)
                return SC_ERR_NOMEM;
            memcpy(dst->name, src->name, src->name_len);
            dst->name[src->name_len] = '\0';
            dst->name_len = src->name_len;
        }
        dst->nrows = src->nrows;
        dst->ncols = src->ncols;
        dst->flags = src->flags;
        if (src->nrows <= 0 || src->ncols <= 0)
            continue;
        cellsz = (size_t)src->nrows * (size_t)src->ncols;
        dst->cells = (ScCell *)calloc(cellsz, sizeof(ScCell));
        if (!dst->cells)
            return SC_ERR_NOMEM;
        dst->cell_cap = cellsz;
        if (src->cells) {
            size_t ci, n = cellsz;
            for (ci = 0; ci < n; ci++) {
                ScCell *dc = &dst->cells[ci];
                const ScCell *sc = &src->cells[ci];
                if (sc_val_copy(&dc->value, &sc->value) != SC_OK)
                    return SC_ERR_NOMEM;
                dc->style_id = sc->style_id;
                dc->dirty = sc->dirty;
                if (sc->formula && sc->formula->text) {
                    rc = copy_formula(b, b->nsheets - 1, dc, sc);
                    if (rc != SC_OK)
                        return rc;
                }
            }
        }
    }
    return rc;
}

int sc_merge_names(sc_workbook *base, const sc_workbook *other)
{
    ScWorkbook *b, *o;
    int i, rc = SC_OK;

    if (!base || !other)
        return SC_ERR_RANGE;
    b = SC_WB(base);
    o = SC_WB(other);

    for (i = 0; i < o->nnames && rc == SC_OK; i++) {
        CsNameDef *src = &o->names[i];
        CsNameDef *nd;
        size_t cap;

        if (sci_resolve_name(b, src->name, strlen(src->name), NULL) == SC_OK)
            continue;
        if (b->nnames >= (int)b->name_cap) {
            cap = b->name_cap ? b->name_cap * 2 : SC_SCOPE_INIT_CAP;
            nd = (CsNameDef *)realloc(b->names, cap * sizeof(CsNameDef));
            if (!nd)
                return SC_ERR_NOMEM;
            b->names = nd;
            b->name_cap = cap;
        }
        nd = &b->names[b->nnames++];
        nd->name = (char *)malloc(strlen(src->name) + 1);
        if (!nd->name)
            return SC_ERR_NOMEM;
        strcpy(nd->name, src->name);
        nd->sheet = src->sheet;
        nd->r0 = src->r0;
        nd->c0 = src->c0;
        nd->r1 = src->r1;
        nd->c1 = src->c1;
    }
    return rc;
}

int sc_merge_styles(sc_workbook *base, const sc_workbook *other)
{
    ScWorkbook *b, *o;
    int i;

    if (!base || !other)
        return SC_ERR_RANGE;
    b = SC_WB(base);
    o = SC_WB(other);
    if (o->nstyles <= 0)
        return SC_OK;

    if (b->nstyles + o->nstyles > (int)b->style_cap) {
        size_t cap = (size_t)b->nstyles + (size_t)o->nstyles;
        CsStyle *ns = (CsStyle *)realloc(b->styles, cap * sizeof(CsStyle));
        if (!ns)
            return SC_ERR_NOMEM;
        b->styles = ns;
        b->style_cap = cap;
    }
    for (i = 0; i < o->nstyles; i++)
        b->styles[b->nstyles++] = o->styles[i];
    return SC_OK;
}

int cs_merge(sc_workbook *base, const sc_workbook *other,
             CsMergeStrategy strategy, CsConflictList *conflicts)
{
    ScWorkbook *b, *o;
    int oi, bi, rc;

    if (!base || !other)
        return SC_ERR_RANGE;
    b = SC_WB(base);
    o = SC_WB(other);
    if (conflicts) {
        conflicts->conflicts = NULL;
        conflicts->count = conflicts->cap = 0;
    }

    rc = sc_merge_sheets(base, other);
    if (rc != SC_OK)
        return rc;
    rc = sc_merge_styles(base, other);
    if (rc != SC_OK)
        return rc;
    rc = sc_merge_names(base, other);
    if (rc != SC_OK)
        return rc;

    for (oi = 0; oi < o->nsheets; oi++) {
        ScSheet *os = &o->sheets[oi];
        ScSheet *bs = find_matching_sheet(b, os->name);
        if (!bs)
            continue;
        for (bi = 0; bi < b->nsheets; bi++) {
            if (&b->sheets[bi] == bs)
                break;
        }
        if (bi >= b->nsheets)
            continue;
        rc = merge_cells(b, bs, os, bi, strategy, conflicts);
        if (rc != SC_OK)
            return rc;
    }
    b->flags |= SC_WB_DIRTY;
    return SC_OK;
}

void sc_merge_free_conflicts(CsConflictList *cl)
{
    size_t i;

    if (!cl)
        return;
    for (i = 0; i < cl->count; i++) {
        sc_val_free(&cl->conflicts[i].ours);
        sc_val_free(&cl->conflicts[i].theirs);
    }
    free(cl->conflicts);
    cl->conflicts = NULL;
    cl->count = cl->cap = 0;
}

int sc_merge_resolve(sc_workbook *wb, const CsConflictList *cl, CsMergeStrategy strategy)
{
    ScWorkbook *w;
    size_t i;
    int rc = SC_OK;

    if (!wb || !cl)
        return SC_ERR_RANGE;
    w = SC_WB(wb);

    for (i = 0; i < cl->count && rc == SC_OK; i++) {
        const CsConflict *cf = &cl->conflicts[i];
        ScCell *cll;
        const ScVal *pick;

        if (cf->sheet < 0 || cf->sheet >= w->nsheets)
            continue;
        rc = ensure_dims(&w->sheets[cf->sheet], cf->row, cf->col);
        if (rc != SC_OK)
            break;
        rc = sci_cell_at(&w->sheets[cf->sheet], cf->row, cf->col, &cll);
        if (rc != SC_OK)
            break;

        if (strategy == SC_MERGE_THEIRS)
            pick = &cf->theirs;
        else if (strategy == SC_MERGE_NEWER)
            pick = cll->style_id ? &cf->ours : &cf->theirs;
        else
            pick = &cf->ours;

        sc_val_free(&cll->value);
        rc = sc_val_copy(&cll->value, pick);
    }
    if (rc == SC_OK)
        w->flags |= SC_WB_DIRTY;
    return rc;
}
