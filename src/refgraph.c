/*
 * refgraph.c — cell reference dependency graph
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

typedef struct {
    int sheet, row, col;
} CsRef;

typedef struct {
    CsRef *deps;
    size_t ndeps, dep_cap;
    CsRef *rdeps;
    size_t nrdeps, rdep_cap;
} CsRefNode;

typedef struct {
    CsRefNode *nodes;
    size_t nnodes, node_cap;
    int *order;
    size_t norder;
} CsRefGraph;

typedef struct KeyMap {
    CsRefGraph *g;
    CsRef *keys;
    struct KeyMap *next;
} KeyMap;

static KeyMap *s_km;
static ScWorkbook *s_wb;

#define REF_SH(wb, cur, ref)  ((ref) < 0 ? (cur) : (ref))

static KeyMap *km_get(CsRefGraph *g, int mk)
{
    KeyMap *m;
    for (m = s_km; m; m = m->next)
        if (m->g == g) return m;
    if (!mk) return NULL;
    m = (KeyMap *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->g = g;
    m->next = s_km;
    s_km = m;
    return m;
}

static int ref_eq(CsRef a, CsRef b)
{
    return a.sheet == b.sheet && a.row == b.row && a.col == b.col;
}

static int ref_push(CsRef **a, size_t *n, size_t *cap, CsRef r)
{
    size_t i;
    for (i = 0; i < *n; i++)
        if (ref_eq((*a)[i], r)) return SC_OK;
    if (*n >= *cap) {
        size_t nc = *cap ? *cap * 2 : 4;
        CsRef *na = (CsRef *)realloc(*a, nc * sizeof(CsRef));
        if (!na) return SC_ERR_NOMEM;
        *a = na;
        *cap = nc;
    }
    (*a)[(*n)++] = r;
    return SC_OK;
}

static int grow(CsRefGraph *g)
{
    KeyMap *m;
    size_t nc;
    CsRefNode *nn;
    CsRef *nk;

    if (g->nnodes < g->node_cap) return SC_OK;
    nc = g->node_cap ? g->node_cap * 2 : 64;
    nn = (CsRefNode *)realloc(g->nodes, nc * sizeof(CsRefNode));
    if (!nn) return SC_ERR_NOMEM;
    memset(nn + g->nnodes, 0, (nc - g->nnodes) * sizeof(CsRefNode));
    g->nodes = nn;
    g->node_cap = nc;
    m = km_get(g, 0);
    if (!m) return SC_ERR_NOMEM;
    nk = (CsRef *)realloc(m->keys, nc * sizeof(CsRef));
    if (!nk) return SC_ERR_NOMEM;
    m->keys = nk;
    return SC_OK;
}

static int ref_to_idx(CsRefGraph *g, CsRef r)
{
    KeyMap *m;
    size_t i;

    m = km_get(g, 1);
    if (!m) return -1;
    for (i = 0; i < g->nnodes; i++)
        if (ref_eq(m->keys[i], r)) return (int)i;
    if (grow(g) != SC_OK) return -1;
    m->keys[g->nnodes] = r;
    return (int)g->nnodes++;
}

static int idx_of(CsRefGraph *g, CsRef r)
{
    KeyMap *m = km_get(g, 0);
    size_t i;
    if (!m) return -1;
    for (i = 0; i < g->nnodes; i++)
        if (ref_eq(m->keys[i], r)) return (int)i;
    return -1;
}

int sci_refgraph_add_edge(CsRefGraph *g, CsRef from, CsRef to)
{
    int fi, ti, rc;
    CsRefNode *nf, *nt;

    if (!g) return SC_ERR_RANGE;
    if (ref_eq(from, to)) return SC_OK;
    fi = ref_to_idx(g, from);
    ti = ref_to_idx(g, to);
    if (fi < 0 || ti < 0) return SC_ERR_NOMEM;
    nf = &g->nodes[fi];
    nt = &g->nodes[ti];
    rc = ref_push(&nf->deps, &nf->ndeps, &nf->dep_cap, to);
    if (rc != SC_OK) return rc;
    return ref_push(&nt->rdeps, &nt->nrdeps, &nt->rdep_cap, from);
}

static void range_edges(CsRefGraph *g, CsRef from, int sh,
                        int r0, int c0, int r1, int c1)
{
    int lr, hr, lc, hc, r, c;
    CsRef dep = { sh, 0, 0 };

    lr = r0 < r1 ? r0 : r1; hr = r0 < r1 ? r1 : r0;
    lc = c0 < c1 ? c0 : c1; hc = c0 < c1 ? c1 : c0;
    for (r = lr; r <= hr; r++)
        for (c = lc; c <= hc; c++) {
            dep.row = r;
            dep.col = c;
            sci_refgraph_add_edge(g, from, dep);
        }
}

static void walk_formula_refs(ScNode *node, CsRef cell, CsRefGraph *g)
{
    CsNameDef *nd = NULL;
    ScNode *ch[3];
    int nch = 0, sh, i;

    if (!node) return;

    switch (node->kind) {
    case SC_NODE_REF:
        sh = REF_SH(s_wb, cell.sheet, node->u.ref.sheet);
        { CsRef dep = { sh, node->u.ref.row, node->u.ref.col };
          sci_refgraph_add_edge(g, cell, dep); }
        return;
    case SC_NODE_RANGE:
        sh = REF_SH(s_wb, cell.sheet, node->u.range.sheet);
        range_edges(g, cell, sh, node->u.range.r0, node->u.range.c0,
                    node->u.range.r1, node->u.range.c1);
        return;
    case SC_NODE_NAME:
        if (s_wb && sci_resolve_name(s_wb, node->u.name.name,
                                      node->u.name.name_len, &nd) == SC_OK) {
            sh = REF_SH(s_wb, cell.sheet, nd->sheet);
            range_edges(g, cell, sh, nd->r0, nd->c0, nd->r1, nd->c1);
        }
        return;
    case SC_NODE_BINARY:
    case SC_NODE_CONCAT:
        ch[0] = node->u.binary.lhs; ch[1] = node->u.binary.rhs; nch = 2; break;
    case SC_NODE_UNARY:
    case SC_NODE_PAREN:
        ch[0] = node->u.unary.operand; nch = 1; break;
    case SC_NODE_CALL:
        for (i = 0; i < node->u.call.nargs; i++)
            walk_formula_refs(node->u.call.args[i], cell, g);
        return;
    case SC_NODE_IF:
        walk_formula_refs(node->u.if_expr.cond, cell, g);
        walk_formula_refs(node->u.if_expr.then_br, cell, g);
        if (node->u.if_expr.else_br)
            walk_formula_refs(node->u.if_expr.else_br, cell, g);
        return;
    case SC_NODE_LIST:
        for (i = 0; i < node->u.list.count; i++)
            walk_formula_refs(node->u.list.elems[i], cell, g);
        return;
    default:
        return;
    }
    for (i = 0; i < nch; i++)
        walk_formula_refs(ch[i], cell, g);
}

int sci_refgraph_node_ref(CsRefGraph *g, int idx, CsRef *out)
{
    KeyMap *m;
    if (!g || !out || idx < 0 || (size_t)idx >= g->nnodes) return SC_ERR_RANGE;
    m = km_get(g, 0);
    if (!m) return SC_ERR_INTERNAL;
    *out = m->keys[idx];
    return SC_OK;
}

int sci_refgraph_build(ScWorkbook *wb, CsRefGraph *g)
{
    int sh;

    if (!wb || !g) return SC_ERR_RANGE;
    memset(g, 0, sizeof(*g));
    if (!km_get(g, 1)) return SC_ERR_NOMEM;

    s_wb = wb;
    for (sh = 0; sh < wb->nsheets; sh++) {
        ScSheet *sheet = &wb->sheets[sh];
        size_t n, i;

        if (!sheet->cells || sheet->nrows <= 0 || sheet->ncols <= 0)
            continue;
        n = (size_t)sheet->nrows * (size_t)sheet->ncols;
        if (n > sheet->cell_cap) n = sheet->cell_cap;

        for (i = 0; i < n; i++) {
            ScCell *cl = &sheet->cells[i];
            CsRef cell;
            int r, c;

            if (!cl->formula || !cl->formula->root) continue;
            r = (int)(i / (size_t)sheet->ncols);
            c = (int)(i % (size_t)sheet->ncols);
            cell.sheet = sh; cell.row = r; cell.col = c;
            if (ref_to_idx(g, cell) < 0) { s_wb = NULL; return SC_ERR_NOMEM; }
            walk_formula_refs(cl->formula->root, cell, g);
        }
    }
    s_wb = NULL;
    return SC_OK;
}

int sci_refgraph_toposort(CsRefGraph *g, int **order, size_t *norder)
{
    int *indeg, *q, *out;
    size_t qh = 0, qt = 0, on = 0, i, j;
    int rc = SC_OK;

    if (!g || !order || !norder) return SC_ERR_RANGE;
    *order = NULL; *norder = 0;
    if (g->nnodes == 0) return SC_OK;

    indeg = (int *)calloc(g->nnodes, sizeof(int));
    q = (int *)malloc(g->nnodes * sizeof(int));
    out = (int *)malloc(g->nnodes * sizeof(int));
    if (!indeg || !q || !out) { rc = SC_ERR_NOMEM; goto done; }

    for (i = 0; i < g->nnodes; i++) indeg[i] = (int)g->nodes[i].ndeps;
    for (i = 0; i < g->nnodes; i++)
        if (indeg[i] == 0) q[qt++] = (int)i;

    while (qh < qt) {
        int cur = q[qh++];
        CsRefNode *nd = &g->nodes[cur];
        out[on++] = cur;
        for (j = 0; j < nd->nrdeps; j++) {
            int ix = idx_of(g, nd->rdeps[j]);
            if (ix >= 0 && --indeg[ix] == 0) q[qt++] = ix;
        }
    }
    if (on != g->nnodes) { rc = SC_ERR_CYCLE; goto done; }

    free(g->order);
    g->order = out;
    g->norder = on;
    out = NULL;
    *order = g->order;
    *norder = g->norder;

done:
    free(indeg);
    free(q);
    free(out);
    return rc;
}

int sci_refgraph_get_dependents(CsRefGraph *g, int sheet, int row, int col,
                                CsRef **out, size_t *count)
{
    CsRef t = { sheet, row, col };
    int ix;

    if (!g || !out || !count) return SC_ERR_RANGE;
    *out = NULL; *count = 0;
    ix = idx_of(g, t);
    if (ix < 0) return SC_ERR_BADREF;
    *out = g->nodes[ix].rdeps;
    *count = g->nodes[ix].nrdeps;
    return SC_OK;
}

void sci_refgraph_free(CsRefGraph *g)
{
    KeyMap **pp, *m;
    size_t i;

    if (!g) return;
    for (i = 0; i < g->nnodes; i++) {
        free(g->nodes[i].deps);
        free(g->nodes[i].rdeps);
    }
    free(g->nodes);
    free(g->order);
    for (pp = &s_km; *pp; ) {
        if ((*pp)->g != g) { pp = &(*pp)->next; continue; }
        m = *pp;
        *pp = m->next;
        free(m->keys);
        free(m);
    }
    memset(g, 0, sizeof(*g));
}
