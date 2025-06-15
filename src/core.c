/*
 * core.c — workbook lifecycle and public API entry points
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

static void free_node(ScNode *node)
{
    int i;

    if (!node)
        return;

    switch (node->kind) {
    case SC_NODE_LIT:
        sc_val_free(&node->u.lit);
        break;
    case SC_NODE_REF:
    case SC_NODE_RANGE:
        break;
    case SC_NODE_BINARY:
    case SC_NODE_CONCAT:
        free_node(node->u.binary.lhs);
        free_node(node->u.binary.rhs);
        break;
    case SC_NODE_UNARY:
    case SC_NODE_PAREN:
        free_node(node->u.unary.operand);
        break;
    case SC_NODE_CALL:
        if (node->u.call.args) {
            for (i = 0; i < node->u.call.nargs; i++)
                free_node(node->u.call.args[i]);
            free(node->u.call.args);
        }
        break;
    case SC_NODE_NAME:
        free(node->u.name.name);
        break;
    case SC_NODE_IF:
        free_node(node->u.if_expr.cond);
        free_node(node->u.if_expr.then_br);
        free_node(node->u.if_expr.else_br);
        break;
    case SC_NODE_LIST:
        if (node->u.list.elems) {
            for (i = 0; i < node->u.list.count; i++)
                free_node(node->u.list.elems[i]);
            free(node->u.list.elems);
        }
        break;
    default:
        break;
    }
    free(node);
}

static void free_formula(ScFormula *fml)
{
    if (!fml)
        return;
    free(fml->text);
    free_node(fml->root);
    free(fml);
}

static void free_cell(ScCell *cell)
{
    if (!cell)
        return;
    sc_val_free(&cell->value);
    free_formula(cell->formula);
    cell->formula = NULL;
}

static void free_sheet(ScSheet *sh)
{
    size_t i, n;

    if (!sh)
        return;
    n = (size_t)sh->nrows * (size_t)sh->ncols;
    if (n > sh->cell_cap)
        n = sh->cell_cap;
    for (i = 0; i < n; i++)
        free_cell(&sh->cells[i]);
    free(sh->cells);
    free(sh->name);
}

static void free_eval_ctx(ScEvalCtx *ctx)
{
    size_t i;

    if (!ctx)
        return;
    for (i = 0; i < ctx->stack_len; i++)
        sc_val_free(&ctx->stack[i]);
    free(ctx->stack);
    for (i = 0; i < ctx->scope_len; i++) {
        free(ctx->scope[i].name);
        sc_val_free(&ctx->scope[i].val);
    }
    free(ctx->scope);
    for (i = 0; i < ctx->memo_len; i++)
        sc_val_free(&ctx->memo[i].val);
    free(ctx->memo);
    sci_buf_free(&ctx->sb);
}

const char *sc_version(void)
{
    return "0.9.2";
}

int sc_workbook_open(const uint8_t *data, size_t len, sc_workbook **out)
{
    ScWorkbook *wb;
    int rc;

    if (!out)
        return SC_ERR_RANGE;
    *out = NULL;
    if (!data && len > 0)
        return SC_ERR_RANGE;

    rc = sci_parse_workbook(data, len, &wb);
    if (rc != SC_OK)
        return rc;

    *out = (sc_workbook *)wb;
    return SC_OK;
}

void sc_workbook_close(sc_workbook *wb)
{
    ScWorkbook *w;
    int i;

    if (!wb)
        return;
    w = SC_WB(wb);

    for (i = 0; i < w->nsheets; i++)
        free_sheet(&w->sheets[i]);
    free(w->sheets);

    for (i = 0; i < w->nnames; i++)
        free(w->names[i].name);
    free(w->names);

    free(w->styles);
    free_eval_ctx(&w->eval);
    free(w->raw);
    free(w);
}

int sc_workbook_sheet_count(const sc_workbook *wb)
{
    return wb ? SC_WB(wb)->nsheets : 0;
}

int sc_workbook_get_sheet(sc_workbook *wb, int idx, sc_sheet **out)
{
    ScWorkbook *w;

    if (!wb || !out)
        return SC_ERR_RANGE;
    *out = NULL;
    w = SC_WB(wb);
    if (idx < 0 || idx >= w->nsheets)
        return SC_ERR_RANGE;
    *out = (sc_sheet *)&w->sheets[idx];
    return SC_OK;
}

int sc_workbook_find_sheet(sc_workbook *wb, const char *name, sc_sheet **out)
{
    ScWorkbook *w;
    int i;

    if (!wb || !name || !out)
        return SC_ERR_RANGE;
    *out = NULL;
    w = SC_WB(wb);
    for (i = 0; i < w->nsheets; i++) {
        if (w->sheets[i].name && strcmp(w->sheets[i].name, name) == 0) {
            *out = (sc_sheet *)&w->sheets[i];
            return SC_OK;
        }
    }
    return SC_ERR_BADREF;
}

const char *sc_sheet_name(const sc_sheet *s)
{
    return s ? SC_SH(s)->name : NULL;
}

void sc_sheet_dims(const sc_sheet *s, int *rows, int *cols)
{
    if (!s)
        return;
    if (rows)
        *rows = SC_SH(s)->nrows;
    if (cols)
        *cols = SC_SH(s)->ncols;
}

int sc_sheet_get_cell(sc_sheet *s, int row, int col, sc_cell **out)
{
    ScCell *cell;

    if (!s || !out)
        return SC_ERR_RANGE;
    *out = NULL;
    if (sci_cell_at(SC_SH(s), row, col, &cell) != SC_OK)
        return SC_ERR_RANGE;
    *out = (sc_cell *)cell;
    return SC_OK;
}

int sc_cell_type(const sc_cell *c)
{
    return c ? SC_CL(c)->value.type : SC_VAL_NIL;
}

int sc_cell_as_num(const sc_cell *c, double *out)
{
    const ScVal *v;

    if (!c || !out)
        return SC_ERR_RANGE;
    v = &SC_CL(c)->value;
    if (v->type == SC_VAL_NUM) {
        *out = v->as.num;
        return SC_OK;
    }
    return sc_val_to_num(v, out);
}

int sc_cell_as_str(const sc_cell *c, const char **out, size_t *len)
{
    const ScVal *v;

    if (!c || !out || !len)
        return SC_ERR_RANGE;
    v = &SC_CL(c)->value;
    if (v->type != SC_VAL_STR)
        return SC_ERR_TYPE;
    *out = v->as.str.s;
    *len = v->as.str.len;
    return SC_OK;
}

int sc_cell_as_bool(const sc_cell *c, int *out)
{
    const ScVal *v;

    if (!c || !out)
        return SC_ERR_RANGE;
    v = &SC_CL(c)->value;
    if (v->type != SC_VAL_BOOL)
        return SC_ERR_TYPE;
    *out = v->as.boolean;
    return SC_OK;
}

int sc_cell_has_formula(const sc_cell *c)
{
    return c && SC_CL(c)->formula != NULL;
}

int sc_workbook_add_cells(sc_workbook *wb, int sheet_idx, int count)
{
    ScWorkbook *w;
    ScSheet *sh;
    size_t old_n, new_n, i;

    if (!wb || sheet_idx < 0 || count <= 0)
        return SC_ERR_RANGE;
    w = SC_WB(wb);
    if (sheet_idx >= w->nsheets)
        return SC_ERR_RANGE;

    sh = &w->sheets[sheet_idx];
    if (sh->ncols <= 0)
        sh->ncols = 1;

    old_n = (size_t)sh->nrows * (size_t)sh->ncols;
    new_n = old_n + (size_t)count;

    if (new_n > sh->cell_cap) {
        size_t cap = sh->cell_cap ? sh->cell_cap : SC_BUF_INIT_CAP;
        ScCell *nc;

        while (cap < new_n)
            cap *= 2;
        nc = (ScCell *)realloc(sh->cells, cap * sizeof(ScCell));
        if (!nc)
            return SC_ERR_NOMEM;
        sh->cells = nc;
        sh->cell_cap = cap;
    }

    for (i = old_n; i < new_n; i++) {
        sc_val_nil(&sh->cells[i].value);
        sh->cells[i].formula = NULL;
        sh->cells[i].dirty = 0;
        sh->cells[i].style_id = 0;
    }

    sh->nrows = (int)((new_n + (size_t)sh->ncols - 1) / (size_t)sh->ncols);
    w->flags |= SC_WB_DIRTY;
    return SC_OK;
}

int sc_workbook_define_name(sc_workbook *wb, const char *name,
                            int sheet, int r0, int c0, int r1, int c1)
{
    ScWorkbook *w;
    CsNameDef *nd;
    char *copy;
    size_t cap;

    if (!wb || !name || !name[0])
        return SC_ERR_RANGE;
    w = SC_WB(wb);

    if (w->nnames >= (int)w->name_cap) {
        cap = w->name_cap ? w->name_cap * 2 : SC_SCOPE_INIT_CAP;
        nd = (CsNameDef *)realloc(w->names, cap * sizeof(CsNameDef));
        if (!nd)
            return SC_ERR_NOMEM;
        w->names = nd;
        w->name_cap = cap;
    }

    copy = (char *)malloc(strlen(name) + 1);
    if (!copy)
        return SC_ERR_NOMEM;
    strcpy(copy, name);

    nd = &w->names[w->nnames++];
    nd->name = copy;
    nd->sheet = sheet;
    nd->r0 = r0;
    nd->c0 = c0;
    nd->r1 = r1;
    nd->c1 = c1;
    w->flags |= SC_WB_DIRTY;
    return SC_OK;
}

int sci_cell_at(ScSheet *s, int row, int col, ScCell **out)
{
    size_t idx;

    if (!s || !out)
        return SC_ERR_RANGE;
    *out = NULL;
    if (row < 0 || col < 0 || row >= s->nrows || col >= s->ncols)
        return SC_ERR_RANGE;
    if (!s->cells)
        return SC_ERR_RANGE;

    idx = SC_CELL_IDX(s, row, col);
    if (idx >= s->cell_cap)
        return SC_ERR_RANGE;
    *out = &s->cells[idx];
    return SC_OK;
}
