/*
 * depsolve.c — dependency-ordered eval, dirty tracking, incremental recalc
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

typedef struct { int sheet, row, col; } CsRef;
typedef struct { CsRef *deps; size_t ndeps; CsRef *rdeps; size_t nrdeps; } CsRefNode;
typedef struct { CsRefNode *nodes; size_t nnodes; int *order; size_t norder; } CsRefGraph;

#define REF_SH(wb, cur, ref)  ((ref) < 0 ? (cur) : (ref))
#define SC_CELL_EVALING       2

int  sci_refgraph_build(ScWorkbook *wb, CsRefGraph *g);
int  sci_refgraph_toposort(CsRefGraph *g, int **order, size_t *norder);
void sci_refgraph_free(CsRefGraph *g);
int  sci_refgraph_get_dependents(CsRefGraph *g, int sheet, int row, int col,
                                 CsRef **out, size_t *count);
int  sci_refgraph_node_ref(CsRefGraph *g, int idx, CsRef *out);

static int is_volatile_formula(ScNode *nd)
{
    int i;
    if (!nd) return 0;
    if (nd->kind == SC_NODE_CALL) {
        if (nd->u.call.func_id == SC_FN_NOW || nd->u.call.func_id == SC_FN_TODAY) return 1;
        for (i = 0; i < nd->u.call.nargs; i++)
            if (is_volatile_formula(nd->u.call.args[i])) return 1;
        return 0;
    }
    if (nd->kind == SC_NODE_BINARY || nd->kind == SC_NODE_CONCAT)
        return is_volatile_formula(nd->u.binary.lhs) || is_volatile_formula(nd->u.binary.rhs);
    if (nd->kind == SC_NODE_UNARY || nd->kind == SC_NODE_PAREN)
        return is_volatile_formula(nd->u.unary.operand);
    if (nd->kind == SC_NODE_IF)
        return is_volatile_formula(nd->u.if_expr.cond) ||
               is_volatile_formula(nd->u.if_expr.then_br) ||
               (nd->u.if_expr.else_br && is_volatile_formula(nd->u.if_expr.else_br));
    for (i = 0; nd->kind == SC_NODE_LIST && i < nd->u.list.count; i++)
        if (is_volatile_formula(nd->u.list.elems[i])) return 1;
    return 0;
}

static int cell_needs_eval(ScWorkbook *wb, int sh, int r, int c)
{
    ScCell *cl;
    if (sh < 0 || sh >= wb->nsheets) return 0;
    if (sci_cell_at(&wb->sheets[sh], r, c, &cl) != SC_OK || !cl->formula) return 0;
    return cl->dirty || cl->value.type == SC_VAL_EMPTY || cl->value.type == SC_VAL_NIL;
}

static int ctx_prep(ScWorkbook *wb)
{
    ScEvalCtx *ctx = &wb->eval;
    size_t i;
    ctx->wb = wb; ctx->had_error = ctx->depth = 0; ctx->error[0] = '\0';
    if (!ctx->stack) {
        ctx->stack_cap = SC_STACK_INIT_CAP;
        ctx->stack = (ScVal *)calloc(ctx->stack_cap, sizeof(ScVal));
        if (!ctx->stack) return SC_ERR_NOMEM;
    } else while (ctx->stack_len) { ScVal t; sc_eval_pop(ctx, &t); sc_val_free(&t); }
    if (!ctx->sb.data) sci_buf_init(&ctx->sb);
    for (i = 0; i < ctx->memo_len; i++) sc_val_free(&ctx->memo[i].val);
    ctx->memo_len = 0;
    return SC_OK;
}

static int in_rect(int sh, int r, int c, int rs, int r0, int c0, int r1, int c1)
{
    int t;
    if (rs != sh) return 0;
    if (r0 > r1) { t = r0; r0 = r1; r1 = t; }
    if (c0 > c1) { t = c0; c0 = c1; c1 = t; }
    return r >= r0 && r <= r1 && c >= c0 && c <= c1;
}

static int ref_touches(ScWorkbook *wb, int cur, ScNode *nd, int sh, int r, int c)
{
    CsNameDef *def;
    int rs, i;
    if (!nd) return 0;
    switch (nd->kind) {
    case SC_NODE_REF:
        rs = REF_SH(wb, cur, nd->u.ref.sheet);
        return rs == sh && nd->u.ref.row == r && nd->u.ref.col == c;
    case SC_NODE_RANGE:
        rs = REF_SH(wb, cur, nd->u.range.sheet);
        return in_rect(sh, r, c, rs, nd->u.range.r0, nd->u.range.c0,
                         nd->u.range.r1, nd->u.range.c1);
    case SC_NODE_NAME:
        if (sci_resolve_name(wb, nd->u.name.name, nd->u.name.name_len, &def) != SC_OK) return 0;
        rs = REF_SH(wb, cur, def->sheet);
        return in_rect(sh, r, c, rs, def->r0, def->c0, def->r1, def->c1);
    case SC_NODE_BINARY:
    case SC_NODE_CONCAT:
        return ref_touches(wb, cur, nd->u.binary.lhs, sh, r, c) ||
               ref_touches(wb, cur, nd->u.binary.rhs, sh, r, c);
    case SC_NODE_UNARY:
    case SC_NODE_PAREN:
        return ref_touches(wb, cur, nd->u.unary.operand, sh, r, c);
    case SC_NODE_CALL:
        for (i = 0; i < nd->u.call.nargs; i++)
            if (ref_touches(wb, cur, nd->u.call.args[i], sh, r, c)) return 1;
        return 0;
    case SC_NODE_IF:
        return ref_touches(wb, cur, nd->u.if_expr.cond, sh, r, c) ||
               ref_touches(wb, cur, nd->u.if_expr.then_br, sh, r, c) ||
               (nd->u.if_expr.else_br && ref_touches(wb, cur, nd->u.if_expr.else_br, sh, r, c));
    case SC_NODE_LIST:
        for (i = 0; i < nd->u.list.count; i++)
            if (ref_touches(wb, cur, nd->u.list.elems[i], sh, r, c)) return 1;
        return 0;
    default: return 0;
    }
}

static void mark_brute(ScWorkbook *wb, int sh, int r, int c)
{
    int si;
    for (si = 0; si < wb->nsheets; si++) {
        ScSheet *s = &wb->sheets[si];
        size_t cap, i;
        if (!s->cells) continue;
        cap = (size_t)s->nrows * (size_t)s->ncols;
        if (cap > s->cell_cap) cap = s->cell_cap;
        for (i = 0; i < cap; i++) {
            ScCell *cl = &s->cells[i];
            int row = (int)(i / (size_t)s->ncols), col = (int)(i % (size_t)s->ncols);
            if (!cl->formula || !cl->formula->root || cl->dirty) continue;
            if (!ref_touches(wb, si, cl->formula->root, sh, r, c)) continue;
            cl->dirty = 1;
            mark_brute(wb, si, row, col);
        }
    }
}

static void mark_graph(ScWorkbook *wb, CsRefGraph *g, int sh, int r, int c)
{
    CsRef *rd; size_t n, i;
    if (sci_refgraph_get_dependents(g, sh, r, c, &rd, &n) != SC_OK) return;
    for (i = 0; i < n; i++) {
        ScCell *cl;
        if (rd[i].sheet < 0 || rd[i].sheet >= wb->nsheets) continue;
        if (sci_cell_at(&wb->sheets[rd[i].sheet], rd[i].row, rd[i].col, &cl) != SC_OK || cl->dirty) continue;
        cl->dirty = 1;
        mark_graph(wb, g, rd[i].sheet, rd[i].row, rd[i].col);
    }
}

void sci_mark_dirty(ScWorkbook *wb, int sheet, int row, int col)
{
    ScCell *cl; CsRefGraph g;
    if (!wb || sheet < 0 || sheet >= wb->nsheets) return;
    if (sci_cell_at(&wb->sheets[sheet], row, col, &cl) != SC_OK) return;
    cl->dirty = 1;
    wb->flags |= SC_WB_DIRTY;
    wb->sheets[sheet].flags &= ~SC_SHEET_CALC_DONE;
    memset(&g, 0, sizeof(g));
    if (sci_refgraph_build(wb, &g) == SC_OK) { mark_graph(wb, &g, sheet, row, col); sci_refgraph_free(&g); }
    else mark_brute(wb, sheet, row, col);
}

int sci_recalc_cell(ScWorkbook *wb, int sheet, int row, int col)
{
    ScEvalCtx *ctx = &wb->eval;
    ScSheet *sh; ScCell *cl; ScVal res, *cached; uint64_t mkey; int rc;
    if (!wb || sheet < 0 || sheet >= wb->nsheets) return SC_ERR_RANGE;
    sh = &wb->sheets[sheet];
    if (sci_cell_at(sh, row, col, &cl) != SC_OK) return SC_ERR_RANGE;
    if (!cl->formula || !cl->formula->root) return SC_OK;
    ctx->wb = wb;
    mkey = SC_MEMO_KEY((uintptr_t)sh, row, col);
    if (!cl->dirty && sci_memo_get(ctx, mkey, &cached) == SC_OK) {
        sc_val_free(&cl->value);
        return sc_val_copy(&cl->value, cached);
    }
    if (cl->dirty == SC_CELL_EVALING) return SC_ERR_CYCLE;
    cl->dirty = SC_CELL_EVALING;
    sc_val_nil(&res);
    rc = sc_eval_node(ctx, cl->formula->root, &res);
    if (rc != SC_OK) { cl->dirty = 1; sc_val_free(&res); return rc; }
    sc_val_free(&cl->value);
    rc = sc_val_copy(&cl->value, &res);
    sc_val_free(&res);
    if (rc != SC_OK) { cl->dirty = 1; return rc; }
    sci_memo_put(ctx, mkey, &cl->value);
    cl->dirty = 0;
    return SC_OK;
}

static int eval_topo(ScWorkbook *wb, CsRefGraph *g, int *ord, size_t n, const int *mask)
{
    size_t i; int rc, last = SC_OK;
    for (i = 0; i < n; i++) {
        CsRef ref; ScCell *cl;
        if (mask && !mask[ord[i]]) continue;
        if (sci_refgraph_node_ref(g, ord[i], &ref) != SC_OK) continue;
        if (ref.sheet < 0 || ref.sheet >= wb->nsheets) continue;
        if (sci_cell_at(&wb->sheets[ref.sheet], ref.row, ref.col, &cl) != SC_OK) continue;
        if (!cl->formula || !cell_needs_eval(wb, ref.sheet, ref.row, ref.col)) continue;
        rc = sci_recalc_cell(wb, ref.sheet, ref.row, ref.col);
        if (rc != SC_OK) last = rc;
    }
    return last;
}

int sci_eval_in_order(ScWorkbook *wb)
{
    CsRefGraph g; int *ord = NULL; size_t n = 0; int rc, last, sh;
    if (!wb) return SC_ERR_RANGE;
    rc = ctx_prep(wb);
    if (rc != SC_OK) return rc;
    memset(&g, 0, sizeof(g));
    if (sci_refgraph_build(wb, &g) != SC_OK || sci_refgraph_toposort(&g, &ord, &n) != SC_OK) {
        sci_refgraph_free(&g);
        return SC_ERR_CYCLE;
    }
    last = eval_topo(wb, &g, ord, n, NULL);
    for (sh = 0; sh < wb->nsheets; sh++)
        if (last == SC_OK) wb->sheets[sh].flags |= SC_SHEET_CALC_DONE;
    sci_refgraph_free(&g);
    wb->flags &= ~SC_WB_DIRTY;
    return wb->eval.had_error ? SC_ERR_INTERNAL : last;
}

void csi_volatile_scan(ScWorkbook *wb)
{
    int sh;
    if (!wb) return;
    for (sh = 0; sh < wb->nsheets; sh++) {
        ScSheet *s = &wb->sheets[sh];
        size_t cap, i;
        if (!s->cells) continue;
        cap = (size_t)s->nrows * (size_t)s->ncols;
        if (cap > s->cell_cap) cap = s->cell_cap;
        for (i = 0; i < cap; i++) {
            ScCell *cl = &s->cells[i];
            if (!cl->formula || !is_volatile_formula(cl->formula->root)) continue;
            sci_mark_dirty(wb, sh, (int)(i / (size_t)s->ncols), (int)(i % (size_t)s->ncols));
        }
    }
}

int sci_eval_batch(ScWorkbook *wb, CsRef *cells, size_t ncells)
{
    CsRefGraph g; int *ord = NULL, *mask = NULL; size_t n = 0, i, j, d, u; int rc, last;
    CsRef k, kk, dep;
    if (!wb) return SC_ERR_RANGE;
    if (ncells > 0 && !cells) return SC_ERR_RANGE;
    rc = ctx_prep(wb);
    if (rc != SC_OK) return rc;
    for (i = 0; i < ncells; i++) sci_mark_dirty(wb, cells[i].sheet, cells[i].row, cells[i].col);
    memset(&g, 0, sizeof(g));
    if (sci_refgraph_build(wb, &g) != SC_OK) return sci_eval_in_order(wb);
    if (sci_refgraph_toposort(&g, &ord, &n) != SC_OK) { sci_refgraph_free(&g); return SC_ERR_CYCLE; }
    mask = (int *)calloc(g.nnodes, sizeof(int));
    if (!mask) { sci_refgraph_free(&g); return SC_ERR_NOMEM; }
    for (i = 0; i < ncells; i++) {
        CsRef t = cells[i];
        for (j = 0; j < g.nnodes; j++) {
            if (sci_refgraph_node_ref(&g, (int)j, &k) != SC_OK) continue;
            if (k.sheet != t.sheet || k.row != t.row || k.col != t.col) continue;
            mask[j] = 1;
            for (d = 0; d < g.nodes[j].ndeps; d++) {
                dep = g.nodes[j].deps[d];
                for (u = 0; u < g.nnodes; u++) {
                    if (sci_refgraph_node_ref(&g, (int)u, &kk) != SC_OK) continue;
                    if (kk.sheet == dep.sheet && kk.row == dep.row && kk.col == dep.col) mask[u] = 1;
                }
            }
        }
    }
    last = eval_topo(wb, &g, ord, n, mask);
    free(mask);
    sci_refgraph_free(&g);
    return last;
}
