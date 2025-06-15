/*
 * validate.c — workbook integrity checks before eval/export
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SC_VIS_NONE = 0, SC_VIS_ACTIVE = 1, SC_VIS_DONE = 2 };
#define SC_ALIGN_MAX  8
#define SC_FMT_ID_MAX 4096
#define EMSG(b, n, ...) do { if ((b) && (n) > 0) snprintf((b), (n), __VA_ARGS__); } while (0)

#define REF_SH(wb, cur, ref) \
    ((ref) < 0 ? (cur) : ((ref) >= (wb)->nsheets ? -1 : (ref)))

#define FOR_EACH_CELL(wb, sh, sheet, r, c, cl)                          \
    for ((sh) = 0; (sh) < (wb)->nsheets; (sh)++)                        \
        for ((sheet) = &(wb)->sheets[(sh)], (r) = 0;                     \
             (sheet)->cells && (r) < (sheet)->nrows; (r)++)             \
            for ((c) = 0; (c) < (sheet)->ncols; (c)++)                 \
                if (((cl) = &(sheet)->cells[SC_CELL_IDX((sheet),(r),(c))]))

static int validate_val(const ScVal *v, char *errbuf, size_t sz)
{
    size_t i;
    if (!v) return SC_ERR_CORRUPT;
    if (v->type < SC_VAL_EMPTY || v->type > SC_VAL_NIL) {
        EMSG(errbuf, sz, "invalid value type %d", v->type);
        return SC_ERR_TYPE;
    }
    if (v->type == SC_VAL_STR && !v->as.str.s) {
        EMSG(errbuf, sz, "string value missing data pointer");
        return SC_ERR_CORRUPT;
    }
    if (v->type == SC_VAL_LIST && v->as.list.count > 0 && !v->as.list.items) {
        EMSG(errbuf, sz, "list has count but NULL items");
        return SC_ERR_CORRUPT;
    }
    for (i = 0; v->type == SC_VAL_LIST && i < v->as.list.count; i++) {
        int rc = validate_val(&v->as.list.items[i], errbuf, sz);
        if (rc != SC_OK) return rc;
    }
    /* TODO: tighten this check — validate SC_VAL_ERR codes */
    return SC_OK;
}

static int chk_cell(const ScWorkbook *wb, int cur, int rs, int row, int col,
                    char *errbuf, size_t sz)
{
    int sh = REF_SH(wb, cur, rs);
    const ScSheet *s;
    if (sh < 0) { EMSG(errbuf, sz, "invalid sheet index %d", rs); return SC_ERR_BADREF; }
    s = &wb->sheets[sh];
    if (row < 0 || col < 0 || row >= s->nrows || col >= s->ncols) {
        EMSG(errbuf, sz, "cell ref OOB (sheet %d, r%d c%d)", sh, row, col);
        return SC_ERR_BADREF;
    }
    return SC_OK;
}

static int chk_range(const ScWorkbook *wb, int cur, int rs,
                     int r0, int c0, int r1, int c1, char *errbuf, size_t sz)
{
    int rc;
    if (r0 > r1 || c0 > c1) {
        EMSG(errbuf, sz, "range ref inverted (%d,%d):(%d,%d)", r0, c0, r1, c1);
        return SC_ERR_BADREF;
    }
    rc = chk_cell(wb, cur, rs, r0, c0, errbuf, sz);
    return (rc != SC_OK) ? rc : chk_cell(wb, cur, rs, r1, c1, errbuf, sz);
}

static int walk_tree(ScWorkbook *wb, int cur, ScNode *node, int mode,
                     uint8_t **marks, char *errbuf, size_t sz);
static int cycle_touch(ScWorkbook *wb, int sh, int r, int c,
                       uint8_t **marks, char *errbuf, size_t sz);

enum { WALK_AST = 1, WALK_REFS, WALK_CYCLE };

static int walk_kids(ScWorkbook *wb, int cur, ScNode **kids, int n, int mode,
                     uint8_t **marks, char *errbuf, size_t sz)
{
    for (; n > 0; n--, kids++) {
        int rc = walk_tree(wb, cur, kids[0], mode, marks, errbuf, sz);
        if (rc != SC_OK) return rc;
    }
    return SC_OK;
}

static int walk_tree(ScWorkbook *wb, int cur, ScNode *node, int mode,
                     uint8_t **marks, char *errbuf, size_t sz)
{
    CsNameDef *nd = NULL;
    ScNode *kids[3];
    int nk = 0, sh, r, c, rc, r0, c0, r1, c1;

    if (!node) {
        if (mode == WALK_AST) { EMSG(errbuf, sz, "NULL AST node"); return SC_ERR_CORRUPT; }
        return SC_OK;
    }

    switch (node->kind) {
    case SC_NODE_LIT:
        return (mode == WALK_AST) ? validate_val(&node->u.lit, errbuf, sz) : SC_OK;
    case SC_NODE_REF:
        if (mode == WALK_CYCLE) {
            sh = REF_SH(wb, cur, node->u.ref.sheet);
            return (sh < 0) ? SC_OK
                : cycle_touch(wb, sh, node->u.ref.row, node->u.ref.col, marks, errbuf, sz);
        }
        if (mode == WALK_REFS)
            return chk_cell(wb, cur, node->u.ref.sheet, node->u.ref.row, node->u.ref.col, errbuf, sz);
        return SC_OK;
    case SC_NODE_RANGE:
        if (mode == WALK_REFS)
            return chk_range(wb, cur, node->u.range.sheet,
                             node->u.range.r0, node->u.range.c0,
                             node->u.range.r1, node->u.range.c1, errbuf, sz);
        if (mode == WALK_CYCLE) {
            sh = REF_SH(wb, cur, node->u.range.sheet);
            r0 = node->u.range.r0; c0 = node->u.range.c0;
            r1 = node->u.range.r1; c1 = node->u.range.c1;
            goto touch_range;
        }
        return SC_OK;
    case SC_NODE_NAME:
        if (mode == WALK_AST) {
            if (!node->u.name.name || node->u.name.name_len == 0) {
                EMSG(errbuf, sz, "name node missing identifier");
                return SC_ERR_CORRUPT;
            }
            return SC_OK;
        }
        if (sci_resolve_name(wb, node->u.name.name, node->u.name.name_len, &nd) != SC_OK) {
            if (mode == WALK_REFS)
                EMSG(errbuf, sz, "undefined name '%.*s'",
                     (int)node->u.name.name_len, node->u.name.name);
            return (mode == WALK_CYCLE) ? SC_OK : SC_ERR_BADREF;
        }
        if (mode == WALK_REFS)
            return chk_range(wb, cur, nd->sheet, nd->r0, nd->c0, nd->r1, nd->c1, errbuf, sz);
        sh = REF_SH(wb, cur, nd->sheet);
        r0 = nd->r0; c0 = nd->c0; r1 = nd->r1; c1 = nd->c1;
        goto touch_range;
    case SC_NODE_BINARY:
    case SC_NODE_CONCAT:
        if (mode == WALK_AST && (!node->u.binary.lhs || !node->u.binary.rhs)) {
            EMSG(errbuf, sz, "binary node missing operand(s)");
            return SC_ERR_CORRUPT;
        }
        kids[0] = node->u.binary.lhs; kids[1] = node->u.binary.rhs; nk = 2; break;
    case SC_NODE_UNARY:
    case SC_NODE_PAREN:
        if (mode == WALK_AST && !node->u.unary.operand) {
            EMSG(errbuf, sz, "unary node missing operand");
            return SC_ERR_CORRUPT;
        }
        kids[0] = node->u.unary.operand; nk = 1; break;
    case SC_NODE_CALL:
        if (mode == WALK_AST) {
            if (node->u.call.func_id <= 0 || node->u.call.func_id >= SC_FN__COUNT) {
                EMSG(errbuf, sz, "bad function id %d", node->u.call.func_id);
                return SC_ERR_FORMAT;
            }
            if (node->u.call.nargs > 0 && !node->u.call.args) {
                EMSG(errbuf, sz, "call node missing args array");
                return SC_ERR_CORRUPT;
            }
        }
        return walk_kids(wb, cur, node->u.call.args, node->u.call.nargs,
                         mode, marks, errbuf, sz);
    case SC_NODE_IF:
        if (mode == WALK_AST && (!node->u.if_expr.cond || !node->u.if_expr.then_br)) {
            EMSG(errbuf, sz, "IF node missing branch");
            return SC_ERR_CORRUPT;
        }
        kids[0] = node->u.if_expr.cond;
        kids[1] = node->u.if_expr.then_br;
        kids[2] = node->u.if_expr.else_br;
        nk = node->u.if_expr.else_br ? 3 : 2;
        break;
    case SC_NODE_LIST:
        if (mode == WALK_AST && node->u.list.count > 0 && !node->u.list.elems) {
            EMSG(errbuf, sz, "list node missing elements");
            return SC_ERR_CORRUPT;
        }
        return walk_kids(wb, cur, node->u.list.elems, node->u.list.count,
                         mode, marks, errbuf, sz);
    default:
        if (mode == WALK_AST) {
            EMSG(errbuf, sz, "unknown node kind %d", node->kind);
            return SC_ERR_CORRUPT;
        }
        return SC_OK;
    }
    return walk_kids(wb, cur, kids, nk, mode, marks, errbuf, sz);

touch_range:
    if (sh < 0) return SC_OK;
    for (r = r0; r <= r1; r++)
        for (c = c0; c <= c1; c++) {
            rc = cycle_touch(wb, sh, r, c, marks, errbuf, sz);
            if (rc != SC_OK) return rc;
        }
    return SC_OK;
}

static int validate_node(ScNode *node, const ScWorkbook *wb, int sheet,
                         char *errbuf, size_t sz)
{
    return walk_tree((ScWorkbook *)wb, sheet, node, WALK_AST, NULL, errbuf, sz);
}

static int validate_sheets(const ScWorkbook *wb, char *errbuf, size_t sz)
{
    int i, j;
    size_t ncells;
    if (!wb) { EMSG(errbuf, sz, "NULL workbook"); return SC_ERR_RANGE; }
    if (wb->nsheets <= 0) {
        EMSG(errbuf, sz, "workbook has no sheets");
        return SC_ERR_FORMAT;
    }
    for (i = 0; i < wb->nsheets; i++) {
        const ScSheet *sh = &wb->sheets[i];
        if (!sh->name || sh->name[0] == '\0') {
            EMSG(errbuf, sz, "sheet %d has no name", i);
            return SC_ERR_FORMAT;
        }
        if (sh->name_len > SC_MAX_SHEET_NAME || strlen(sh->name) > SC_MAX_SHEET_NAME) {
            EMSG(errbuf, sz, "sheet %d name too long", i);
            return SC_ERR_LIMIT;
        }
        if (sh->nrows <= 0 || sh->ncols <= 0) {
            EMSG(errbuf, sz, "sheet '%s' has invalid dims %dx%d", sh->name, sh->nrows, sh->ncols);
            return SC_ERR_RANGE;
        }
        ncells = (size_t)sh->nrows * (size_t)sh->ncols;
        if (ncells > 0 && !sh->cells) {
            EMSG(errbuf, sz, "sheet '%s' cells not allocated", sh->name);
            return SC_ERR_CORRUPT;
        }
        for (j = i + 1; j < wb->nsheets; j++)
            if (wb->sheets[j].name && strcmp(sh->name, wb->sheets[j].name) == 0) {
                EMSG(errbuf, sz, "duplicate sheet name '%s'", sh->name);
                return SC_ERR_FORMAT;
            }
    }
    return SC_OK;
}

static int validate_cells(const ScWorkbook *wb, char *errbuf, size_t sz)
{
    int sh, r, c, rc;
    const ScSheet *sheet;
    const ScCell *cl;

    FOR_EACH_CELL(wb, sh, sheet, r, c, cl) {
        rc = validate_val(&cl->value, errbuf, sz);
        if (rc != SC_OK) {
            EMSG(errbuf, sz, "sheet '%s' r%d c%d: bad value", sheet->name, r, c);
            return rc;
        }
        if (wb->nstyles > 0 && cl->style_id >= (uint32_t)wb->nstyles) {
            EMSG(errbuf, sz, "sheet '%s' r%d c%d: style_id %u out of range",
                 sheet->name, r, c, cl->style_id);
            return SC_ERR_RANGE;
        }
        if (wb->nstyles == 0 && cl->style_id != 0) {
            EMSG(errbuf, sz, "sheet '%s' r%d c%d: style_id set but no styles", sheet->name, r, c);
            return SC_ERR_RANGE;
        }
    }
    return SC_OK;
}

static int validate_formulas(const ScWorkbook *wb, char *errbuf, size_t sz)
{
    int sh, r, c, rc;
    const ScSheet *sheet;
    const ScCell *cl;

    FOR_EACH_CELL(wb, sh, sheet, r, c, cl) {
        const ScFormula *fml = cl->formula;
        if (!fml) continue;
        if (!fml->root || !fml->text) {
            EMSG(errbuf, sz, "sheet '%s' r%d c%d: incomplete formula", sheet->name, r, c);
            return SC_ERR_CORRUPT;
        }
        if (fml->text_len > SC_MAX_FORMULA_LEN) {
            EMSG(errbuf, sz, "sheet '%s' r%d c%d: formula text too long", sheet->name, r, c);
            return SC_ERR_LIMIT;
        }
        rc = validate_node(fml->root, wb, sh, errbuf, sz);
        if (rc != SC_OK) {
            EMSG(errbuf, sz, "sheet '%s' r%d c%d formula AST invalid", sheet->name, r, c);
            return rc;
        }
    }
    return SC_OK;
}

static int cycle_touch(ScWorkbook *wb, int sh, int r, int c,
                       uint8_t **marks, char *errbuf, size_t sz)
{
    ScSheet *sheet;
    ScCell *cl;
    size_t idx;
    int rc;

    if (sh < 0 || sh >= wb->nsheets) return SC_OK;
    sheet = &wb->sheets[sh];
    if (r < 0 || c < 0 || r >= sheet->nrows || c >= sheet->ncols) return SC_OK;
    if (!sheet->cells) return SC_OK;
    idx = SC_CELL_IDX(sheet, r, c);
    if (idx >= sheet->cell_cap) return SC_OK;
    if (marks[sh][idx] == SC_VIS_ACTIVE) {
        EMSG(errbuf, sz, "circular reference at sheet '%s' r%d c%d", sheet->name, r, c);
        return SC_ERR_CYCLE;
    }
    if (marks[sh][idx] == SC_VIS_DONE) return SC_OK;
    cl = &sheet->cells[idx];
    if (!cl->formula || !cl->formula->root) {
        marks[sh][idx] = SC_VIS_DONE;
        return SC_OK;
    }
    marks[sh][idx] = SC_VIS_ACTIVE;
    rc = walk_tree(wb, sh, cl->formula->root, WALK_CYCLE, marks, errbuf, sz);
    marks[sh][idx] = (rc == SC_OK) ? SC_VIS_DONE : SC_VIS_NONE;
    return rc;
}

static int validate_refs(const ScWorkbook *wb, char *errbuf, size_t sz)
{
    int sh, r, c, rc;
    const ScSheet *sheet;
    const ScCell *cl;
    ScWorkbook *mut = (ScWorkbook *)wb;

    FOR_EACH_CELL(wb, sh, sheet, r, c, cl) {
        if (!cl->formula || !cl->formula->root) continue;
        rc = walk_tree(mut, sh, cl->formula->root, WALK_REFS, NULL, errbuf, sz);
        if (rc != SC_OK) {
            EMSG(errbuf, sz, "sheet '%s' r%d c%d: bad reference", sheet->name, r, c);
            return rc;
        }
    }
    return SC_OK;
}

static int validate_names(const ScWorkbook *wb, char *errbuf, size_t sz)
{
    int i, j;
    for (i = 0; i < wb->nnames; i++) {
        const CsNameDef *nd = &wb->names[i];
        const ScSheet *sh;
        if (!nd->name || nd->name[0] == '\0' || strlen(nd->name) > SC_MAX_SHEET_NAME) {
            EMSG(errbuf, sz, "named range %d has bad name", i);
            return nd->name && nd->name[0] ? SC_ERR_LIMIT : SC_ERR_FORMAT;
        }
        if (nd->sheet < -1 || nd->sheet >= wb->nsheets) {
            EMSG(errbuf, sz, "name '%s' bad sheet index %d", nd->name, nd->sheet);
            return SC_ERR_BADREF;
        }
        if (nd->r0 > nd->r1 || nd->c0 > nd->c1) {
            EMSG(errbuf, sz, "name '%s' inverted range", nd->name);
            return SC_ERR_RANGE;
        }
        if (nd->sheet >= 0) {
            sh = &wb->sheets[nd->sheet];
            if (nd->r0 < 0 || nd->c0 < 0 || nd->r1 >= sh->nrows || nd->c1 >= sh->ncols) {
                EMSG(errbuf, sz, "name '%s' range OOB", nd->name);
                return SC_ERR_BADREF;
            }
        }
        for (j = i + 1; j < wb->nnames; j++)
            if (wb->names[j].name && nd->sheet == wb->names[j].sheet &&
                strcmp(nd->name, wb->names[j].name) == 0) {
                EMSG(errbuf, sz, "duplicate name '%s' in same scope", nd->name);
                return SC_ERR_FORMAT;
            }
    }
    return SC_OK;
}

static int validate_styles(const ScWorkbook *wb, char *errbuf, size_t sz)
{
    int i;
    if (wb->nstyles < 0) { EMSG(errbuf, sz, "negative style count"); return SC_ERR_CORRUPT; }
    if (wb->nstyles > 0 && !wb->styles) {
        EMSG(errbuf, sz, "styles table missing");
        return SC_ERR_CORRUPT;
    }
    for (i = 0; i < wb->nstyles; i++) {
        if (wb->styles[i].align > SC_ALIGN_MAX) {
            EMSG(errbuf, sz, "style %d: align out of range", i);
            return SC_ERR_FORMAT;
        }
        if (wb->styles[i].fmt_id > SC_FMT_ID_MAX) {
            EMSG(errbuf, sz, "style %d: fmt_id %u suspicious", i, wb->styles[i].fmt_id);
            return SC_ERR_FORMAT;
        }
        // TODO: tighten this check — font_id / border_mask
    }
    return SC_OK;
}

static int validate_cycles(const ScWorkbook *wb, char *errbuf, size_t sz)
{
    ScWorkbook *mut = (ScWorkbook *)wb;
    uint8_t **marks = NULL;
    int sh, r, c, rc = SC_OK;
    size_t n;

    marks = (uint8_t **)calloc((size_t)wb->nsheets, sizeof(uint8_t *));
    if (!marks) { EMSG(errbuf, sz, "cycle detection: out of memory"); return SC_ERR_NOMEM; }
    for (sh = 0; sh < wb->nsheets; sh++) {
        n = (size_t)wb->sheets[sh].nrows * (size_t)wb->sheets[sh].ncols;
        if (!n) continue;
        marks[sh] = (uint8_t *)calloc(n, 1);
        if (!marks[sh]) { EMSG(errbuf, sz, "cycle marks alloc failed"); rc = SC_ERR_NOMEM; goto done; }
    }
    for (sh = 0; sh < wb->nsheets && rc == SC_OK; sh++) {
        const ScSheet *sheet = &wb->sheets[sh];
        if (!sheet->cells || !marks[sh]) continue;
        for (r = 0; r < sheet->nrows && rc == SC_OK; r++)
            for (c = 0; c < sheet->ncols && rc == SC_OK; c++)
                if (marks[sh][SC_CELL_IDX(sheet, r, c)] == SC_VIS_NONE)
                    rc = cycle_touch(mut, sh, r, c, marks, errbuf, sz);
    }
done:
    for (sh = 0; sh < wb->nsheets; sh++) free(marks[sh]);
    free(marks);
    return rc;
}

int sc_validate(const sc_workbook *wb, char *errbuf, size_t errbuf_sz)
{
    const ScWorkbook *w;
    int rc;

    if (errbuf && errbuf_sz > 0) errbuf[0] = '\0';
    if (!wb) { EMSG(errbuf, errbuf_sz, "NULL workbook pointer"); return SC_ERR_RANGE; }
    w = SC_WB(wb);
    if ((rc = validate_sheets(w, errbuf, errbuf_sz)) != SC_OK) return rc;
    if ((rc = validate_cells(w, errbuf, errbuf_sz)) != SC_OK) return rc;
    if ((rc = validate_formulas(w, errbuf, errbuf_sz)) != SC_OK) return rc;
    if ((rc = validate_refs(w, errbuf, errbuf_sz)) != SC_OK) return rc;
    if ((rc = validate_names(w, errbuf, errbuf_sz)) != SC_OK) return rc;
    if ((rc = validate_styles(w, errbuf, errbuf_sz)) != SC_OK) return rc;
    if ((rc = validate_cycles(w, errbuf, errbuf_sz)) != SC_OK) return rc;
    return SC_OK;
}
