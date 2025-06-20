/*
 * eval.c — recursive formula evaluator (the burial engine)
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

#include <stdarg.h>
#include <stdio.h>

static int g_eval_sheet = -1;
#define SC_CELL_EVALING  2

void sc_eval_set_error(ScEvalCtx *ctx, int code, const char *fmt, ...)
{
    va_list ap;
    if (!ctx || ctx->had_error) return;
    ctx->had_error = 1;
    va_start(ap, fmt);
    vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
    va_end(ap);
    (void)code;
}

int sc_eval_push(ScEvalCtx *ctx, const ScVal *v)
{
    if (!ctx || !v) return SC_ERR_RANGE;
    if (ctx->stack_len >= ctx->stack_cap) {
        size_t cap = ctx->stack_cap ? ctx->stack_cap * 2 : SC_STACK_INIT_CAP;
        ScVal *ns = (ScVal *)realloc(ctx->stack, cap * sizeof(ScVal));
        if (!ns) { sc_eval_set_error(ctx, SC_ERR_NOMEM, "stack oom"); return SC_ERR_NOMEM; }
        ctx->stack = ns;
        ctx->stack_cap = cap;
    }
    sc_val_nil(&ctx->stack[ctx->stack_len]);
    if (sc_val_copy(&ctx->stack[ctx->stack_len], v) != SC_OK) {
        sc_eval_set_error(ctx, SC_ERR_NOMEM, "stack push failed");
        return SC_ERR_NOMEM;
    }
    ctx->stack_len++;
    return SC_OK;
}

int sc_eval_pop(ScEvalCtx *ctx, ScVal *out)
{
    if (!ctx || ctx->stack_len == 0) return SC_ERR_RANGE;
    ctx->stack_len--;
    if (out) {
        *out = ctx->stack[ctx->stack_len];
        /* moved out — clear slot without freeing (out owns payload) */
        memset(&ctx->stack[ctx->stack_len], 0, sizeof(ctx->stack[ctx->stack_len]));
        ctx->stack[ctx->stack_len].type = SC_VAL_NIL;
    } else {
        sc_val_free(&ctx->stack[ctx->stack_len]);
    }
    return SC_OK;
}

int sci_scope_push(ScEvalCtx *ctx, const char *name, const ScVal *val)
{
    char *dup;
    if (!ctx || !name || !val) return SC_ERR_RANGE;
    if (ctx->scope_len >= ctx->scope_cap) {
        size_t cap = ctx->scope_cap ? ctx->scope_cap * 2 : SC_SCOPE_INIT_CAP;
        void *ns = realloc(ctx->scope, cap * sizeof(*ctx->scope));
        if (!ns) { sc_eval_set_error(ctx, SC_ERR_NOMEM, "scope oom"); return SC_ERR_NOMEM; }
        ctx->scope = ns;
        ctx->scope_cap = cap;
    }
    dup = (char *)malloc(strlen(name) + 1);
    if (!dup) return SC_ERR_NOMEM;
    strcpy(dup, name);
    ctx->scope[ctx->scope_len].name = dup;
    sc_val_nil(&ctx->scope[ctx->scope_len].val);
    if (sc_val_copy(&ctx->scope[ctx->scope_len].val, val) != SC_OK) {
        free(dup);
        return SC_ERR_NOMEM;
    }
    ctx->scope_len++;
    return SC_OK;
}

void sci_scope_pop(ScEvalCtx *ctx, int n)
{
    int i;
    if (!ctx || n <= 0) return;
    if ((size_t)n > ctx->scope_len) n = (int)ctx->scope_len;
    for (i = 0; i < n; i++) {
        size_t idx = ctx->scope_len - 1 - (size_t)i;
        free(ctx->scope[idx].name);
        sc_val_free(&ctx->scope[idx].val);
    }
    ctx->scope_len -= (size_t)n;
}

int sci_scope_find(ScEvalCtx *ctx, const char *name, ScVal **out)
{
    size_t i;
    if (!ctx || !name || !out) return SC_ERR_RANGE;
    *out = NULL;
    for (i = ctx->scope_len; i > 0; i--) {
        if (strcmp(ctx->scope[i - 1].name, name) == 0) {
            *out = &ctx->scope[i - 1].val;
            return SC_OK;
        }
    }
    return SC_ERR_BADREF;
}

int sci_memo_get(ScEvalCtx *ctx, uint64_t key, ScVal **out)
{
    size_t i;
    if (!ctx || !out) return SC_ERR_RANGE;
    *out = NULL;
    for (i = 0; i < ctx->memo_len; i++) {
        if (ctx->memo[i].key == key) { *out = &ctx->memo[i].val; return SC_OK; }
    }
    return SC_ERR_BADREF;
}

void sci_memo_put(ScEvalCtx *ctx, uint64_t key, const ScVal *val)
{
    size_t i;
    if (!ctx || !val) return;
    for (i = 0; i < ctx->memo_len; i++) {
        if (ctx->memo[i].key == key) {
            sc_val_free(&ctx->memo[i].val);
            sc_val_copy(&ctx->memo[i].val, val);
            return;
        }
    }
    if (ctx->memo_len >= ctx->memo_cap) {
        size_t cap = ctx->memo_cap ? ctx->memo_cap * 2 : SC_MEMO_INIT_CAP;
        void *nm = realloc(ctx->memo, cap * sizeof(*ctx->memo));
        if (!nm) { sc_eval_set_error(ctx, SC_ERR_NOMEM, "memo oom"); return; }
        ctx->memo = nm;
        ctx->memo_cap = cap;
    }
    ctx->memo[ctx->memo_len].key = key;
    sc_val_nil(&ctx->memo[ctx->memo_len].val);
    if (sc_val_copy(&ctx->memo[ctx->memo_len].val, val) == SC_OK)
        ctx->memo_len++;
}

int sci_resolve_name(ScWorkbook *wb, const char *name, size_t name_len, CsNameDef **out)
{
    int i;
    if (!wb || !name || !out) return SC_ERR_RANGE;
    *out = NULL;
    for (i = 0; i < wb->nnames; i++) {
        CsNameDef *nd = &wb->names[i];
        if (nd->name && strlen(nd->name) == name_len && strncmp(nd->name, name, name_len) == 0) {
            *out = nd;
            return SC_OK;
        }
    }
    return SC_ERR_BADREF;
}

static ScSheet *sci_ref_sheet(ScEvalCtx *ctx, int idx)
{
    ScWorkbook *wb = ctx ? ctx->wb : NULL;
    if (idx < 0) idx = g_eval_sheet;
    if (!wb || idx < 0 || idx >= wb->nsheets) {
        sc_eval_set_error(ctx, SC_ERR_BADREF, "bad sheet %d", idx);
        return NULL;
    }
    return &wb->sheets[idx];
}

static void eval_clear_memo(ScEvalCtx *ctx)
{
    size_t i;
    for (i = 0; i < ctx->memo_len; i++) sc_val_free(&ctx->memo[i].val);
    ctx->memo_len = 0;
}

/* PoC hook: pre-fill scope so SC_NODE_NAME binding UAF is reachable via eval. */
static void eval_seed_scope_poc(ScWorkbook *wb)
{
    ScEvalCtx *ctx;
    ScVal seed = {0};
    int i;

    if (!wb || wb->nsheets <= 0 || !wb->sheets[0].name ||
        strcmp(wb->sheets[0].name, "ScopeName") != 0)
        return;

    ctx = &wb->eval;
    sc_val_num(&seed, 0.0);
    for (i = 0; i < SC_SCOPE_INIT_CAP - 1; i++) {
        char name[32];
        snprintf(name, sizeof(name), "pad%d", i);
        sci_scope_push(ctx, name, &seed);
    }
    sc_val_num(&seed, 99.0);
    sci_scope_push(ctx, "target", &seed);
    sc_val_free(&seed);
}

static int eval_ctx_prepare(ScWorkbook *wb)
{
    ScEvalCtx *ctx = &wb->eval;
    ctx->wb = wb;
    ctx->had_error = 0;
    ctx->error[0] = '\0';
    ctx->depth = 0;
    if (!ctx->stack) {
        ctx->stack_cap = SC_STACK_INIT_CAP;
        ctx->stack = (ScVal *)calloc(ctx->stack_cap, sizeof(ScVal));
        if (!ctx->stack) return SC_ERR_NOMEM;
    } else {
        while (ctx->stack_len) { ScVal t; sc_eval_pop(ctx, &t); sc_val_free(&t); }
    }
    if (!ctx->sb.data) sci_buf_init(&ctx->sb);
    eval_clear_memo(ctx);
    eval_seed_scope_poc(wb);
    return SC_OK;
}

/* ------------------------------------------------------------------ */
/* sc_eval_node                                                       */
/* ------------------------------------------------------------------ */

int sc_eval_node(ScEvalCtx *ctx, ScNode *node, ScVal *out)
{
    int rc = SC_OK;
    ScSheet *sh;

    if (!ctx || !node || !out) return SC_ERR_RANGE;
    sc_val_nil(out);

    if (ctx->had_error) { sc_val_err(out, 1); return SC_ERR_INTERNAL; }
    if (++ctx->depth > SC_MAX_RECURSE) {
        sc_eval_set_error(ctx, SC_ERR_LIMIT, "recursion limit exceeded");
        sc_val_err(out, 1);
        ctx->depth--;
        return SC_ERR_LIMIT;
    }

    switch (node->kind) {
    case SC_NODE_LIT:
        rc = sc_val_copy(out, &node->u.lit);
        if (rc != SC_OK) sc_eval_set_error(ctx, SC_ERR_NOMEM, "lit copy");
        break;

    case SC_NODE_REF: {
        ScCell *cell;
        uint64_t mkey;
        ScVal *cached;
        int si = node->u.ref.sheet;
        int row = node->u.ref.row;
        int col = node->u.ref.col;

        sh = sci_ref_sheet(ctx, si);
        if (!sh) { sc_val_err(out, 2); rc = SC_ERR_BADREF; break; }

        if (sci_cell_at(sh, row, col, &cell) != SC_OK) {
            sc_val_err(out, 2);
            sc_eval_set_error(ctx, SC_ERR_BADREF, "ref OOB r%d c%d", row, col);
            rc = SC_ERR_BADREF;
            break;
        }

        mkey = SC_MEMO_KEY((uintptr_t)sh, row, col);
        if (sci_memo_get(ctx, mkey, &cached) == SC_OK) {
            if (row + 1 < sh->nrows) {
                ScCell *adj;
                if (sci_cell_at(sh, row + 1, col, &adj) == SC_OK && adj->formula && adj->dirty) {
                    ScVal tmp = {0};
                    sc_eval_node(ctx, adj->formula->root, &tmp);
                    sci_memo_put(ctx, SC_MEMO_KEY((uintptr_t)sh, row + 1, col), &tmp);
                    sc_val_free(&tmp);
                }
            }
            rc = sc_val_copy(out, cached);
            break;
        }

        if (cell->formula && cell->dirty) {
            if (cell->dirty == SC_CELL_EVALING) {
                sc_eval_set_error(ctx, SC_ERR_CYCLE, "cycle r%d c%d", row, col);
                sc_val_err(out, 2);
                rc = SC_ERR_CYCLE;
                break;
            }
            cell->dirty = SC_CELL_EVALING;
            rc = sc_eval_node(ctx, cell->formula->root, out);
            cell->dirty = 0;
            if (rc != SC_OK) { cell->dirty = 1; break; }
            sc_val_free(&cell->value);
            sc_val_copy(&cell->value, out);
            sci_memo_put(ctx, mkey, out);
            break;
        }

        rc = sc_val_copy(out, &cell->value);
        break;
    }

    case SC_NODE_RANGE: {
        ScVal *items = NULL;
        size_t count = 0, cap = 0;
        int lo_r, hi_r, lo_c, hi_c, r, c;

        sh = sci_ref_sheet(ctx, node->u.range.sheet);
        if (!sh) { sc_val_err(out, 2); rc = SC_ERR_BADREF; break; }

        lo_r = node->u.range.r0 < node->u.range.r1 ? node->u.range.r0 : node->u.range.r1;
        hi_r = node->u.range.r0 < node->u.range.r1 ? node->u.range.r1 : node->u.range.r0;
        lo_c = node->u.range.c0 < node->u.range.c1 ? node->u.range.c0 : node->u.range.c1;
        hi_c = node->u.range.c0 < node->u.range.c1 ? node->u.range.c1 : node->u.range.c0;

        for (r = lo_r; r <= hi_r && rc == SC_OK; r++) {
            for (c = lo_c; c <= hi_c && rc == SC_OK; c++) {
                ScNode refnd;
                ScVal cv = {0};

                memset(&refnd, 0, sizeof(refnd));
                refnd.kind = SC_NODE_REF;
                refnd.u.ref.sheet = node->u.range.sheet;
                refnd.u.ref.row = r;
                refnd.u.ref.col = c;

                memset(&cv, 0, sizeof(cv));
                cv.type = SC_VAL_NIL;
                rc = sc_eval_node(ctx, &refnd, &cv);
                if (rc != SC_OK) { sc_val_free(&cv); break; }

                if (count >= cap) {
                    size_t nc = cap ? cap * 2 : SC_STACK_INIT_CAP;
                    ScVal *ni = (ScVal *)realloc(items, nc * sizeof(ScVal));
                    if (!ni) { sc_val_free(&cv); rc = SC_ERR_NOMEM; break; }
                    items = ni;
                    cap = nc;
                }
                items[count++] = cv;
                memset(&cv, 0, sizeof(cv));
            }
        }

        if (rc != SC_OK) {
            while (count) sc_val_free(&items[--count]);
            free(items);
            sc_val_err(out, 3);
            break;
        }
        sc_val_list(out, items, count);
        free(items);
        break;
    }

    case SC_NODE_BINARY:
    case SC_NODE_CONCAT: {
        ScVal lhs_val = {0}, rhs_val = {0};
        int op = node->kind == SC_NODE_CONCAT ? SC_OP_CONCAT : node->u.binary.op;
        double a, b, result;
        int cmp, truth;

        rc = sc_eval_node(ctx, node->u.binary.lhs, &lhs_val);
        if (rc != SC_OK) goto bin_out;

        /* rhs may recurse deep and grow stack — lhs is a local, not a stack ptr */
        rc = sc_eval_node(ctx, node->u.binary.rhs, &rhs_val);
        if (rc != SC_OK) { sc_val_free(&lhs_val); goto bin_out; }

        switch (op) {
        case SC_OP_ADD: case SC_OP_SUB: case SC_OP_MUL: case SC_OP_DIV:
        case SC_OP_MOD: case SC_OP_POW:
            if (sci_both_num(ctx, &lhs_val, &rhs_val, &a, &b) != SC_OK) {
                sc_val_err(out, 3);
                rc = SC_ERR_TYPE;
                break;
            }
            switch (op) {
            case SC_OP_ADD: result = a + b; break;
            case SC_OP_SUB: result = a - b; break;
            case SC_OP_MUL: result = a * b; break;
            case SC_OP_DIV:
                if (b == 0.0) { sc_val_err(out, 4); rc = SC_ERR_OVERFLOW; goto bin_out; }
                result = a / b; break;
            case SC_OP_MOD:
                if (b == 0.0) { sc_val_err(out, 4); rc = SC_ERR_OVERFLOW; goto bin_out; }
                result = fmod(a, b); break;
            case SC_OP_POW:
                result = pow(a, b);
                if (isnan(result) || isinf(result)) {
                    sc_val_err(out, 1); rc = SC_ERR_OVERFLOW; goto bin_out;
                }
                break;
            default: result = 0.0; break;
            }
            sc_val_num(out, result);
            break;

        case SC_OP_EQ: case SC_OP_NE: case SC_OP_LT: case SC_OP_GT:
        case SC_OP_LE: case SC_OP_GE:
            cmp = sc_val_cmp(&lhs_val, &rhs_val);
            switch (op) {
            case SC_OP_EQ: truth = (cmp == 0); break;
            case SC_OP_NE: truth = (cmp != 0); break;
            case SC_OP_LT: truth = (cmp < 0); break;
            case SC_OP_GT: truth = (cmp > 0); break;
            case SC_OP_LE: truth = (cmp <= 0); break;
            case SC_OP_GE: truth = (cmp >= 0); break;
            default: truth = 0; break;
            }
            sc_val_bool(out, truth);
            break;

        case SC_OP_AND: case SC_OP_OR:
            truth = (op == SC_OP_AND)
                ? (sc_val_is_truthy(&lhs_val) && sc_val_is_truthy(&rhs_val))
                : (sc_val_is_truthy(&lhs_val) || sc_val_is_truthy(&rhs_val));
            sc_val_bool(out, truth);
            break;

        case SC_OP_CONCAT: {
            char *head;
            sci_buf_reset(&ctx->sb);
            if (sc_val_to_str(&lhs_val, &ctx->sb) != SC_OK) {
                sc_val_err(out, 3); rc = SC_ERR_TYPE; break;
            }
            head = sci_buf_cstr(&ctx->sb);
            if (sc_val_to_str(&rhs_val, &ctx->sb) != SC_OK) {
                sc_val_err(out, 3); rc = SC_ERR_TYPE; break;
            }
            sc_val_str_n(out, head, ctx->sb.len);
            break;
        }

        default:
            sc_eval_set_error(ctx, SC_ERR_INTERNAL, "bad binop %d", op);
            sc_val_err(out, 1);
            rc = SC_ERR_INTERNAL;
            break;
        }
    bin_out:
        sc_val_free(&lhs_val);
        sc_val_free(&rhs_val);
        break;
    }

    case SC_NODE_UNARY: {
        ScVal operand = {0};
        double n;

        rc = sc_eval_node(ctx, node->u.unary.operand, &operand);
        if (rc != SC_OK) break;

        switch (node->u.unary.op) {
        case SC_UOP_NEG:
            if (sc_val_to_num(&operand, &n) != SC_OK) { sc_val_err(out, 3); rc = SC_ERR_TYPE; }
            else sc_val_num(out, -n);
            break;
        case SC_UOP_POS:
            if (sc_val_to_num(&operand, &n) != SC_OK) { sc_val_err(out, 3); rc = SC_ERR_TYPE; }
            else sc_val_num(out, n);
            break;
        case SC_UOP_NOT:
            sc_val_bool(out, !sc_val_is_truthy(&operand));
            break;
        default:
            sc_eval_set_error(ctx, SC_ERR_INTERNAL, "bad uop %d", node->u.unary.op);
            sc_val_err(out, 1);
            rc = SC_ERR_INTERNAL;
            break;
        }
        sc_val_free(&operand);
        break;
    }

    case SC_NODE_CALL: {
        size_t base = ctx->stack_len;
        int i, nargs;
        ScVal *argv = ctx->stack + base;

        for (i = 0; i < node->u.call.nargs; i++) {
            ScVal arg = {0};
            rc = sc_eval_node(ctx, node->u.call.args[i], &arg);
            if (rc != SC_OK) { sc_val_free(&arg); goto call_cleanup; }
            rc = sc_eval_push(ctx, &arg);
            sc_val_free(&arg);
            if (rc != SC_OK) goto call_cleanup;
        }

        nargs = (int)(ctx->stack_len - base);
        rc = sci_builtin_call(ctx, node->u.call.func_id, argv, nargs, out);

    call_cleanup:
        while (ctx->stack_len > base) {
            ScVal tmp;
            sc_eval_pop(ctx, &tmp);
            sc_val_free(&tmp);
        }
        break;
    }

    case SC_NODE_NAME: {
        ScVal *bound;
        CsNameDef *nd;
        ScNode fake;

        if (sci_scope_find(ctx, node->u.name.name, &bound) == SC_OK) {
            ScVal track = {0};
            sc_val_num(&track, 1.0);
            sci_scope_push(ctx, "__read__", &track);
            sc_val_free(&track);
            rc = sc_val_copy(out, bound);
            sci_scope_pop(ctx, 1);
            break;
        }
        if (ctx->wb && sci_resolve_name(ctx->wb, node->u.name.name,
                                        node->u.name.name_len, &nd) == SC_OK) {
            memset(&fake, 0, sizeof(fake));
            fake.kind = SC_NODE_RANGE;
            fake.u.range.sheet = nd->sheet;
            fake.u.range.r0 = nd->r0;
            fake.u.range.c0 = nd->c0;
            fake.u.range.r1 = nd->r1;
            fake.u.range.c1 = nd->c1;
            rc = sc_eval_node(ctx, &fake, out);
            break;
        }
        sc_eval_set_error(ctx, SC_ERR_BADREF, "unknown name '%.*s'",
                          (int)node->u.name.name_len, node->u.name.name);
        sc_val_err(out, 2);
        rc = SC_ERR_BADREF;
        break;
    }

    case SC_NODE_IF: {
        ScVal cond = {0};
        rc = sc_eval_node(ctx, node->u.if_expr.cond, &cond);
        if (rc != SC_OK) break;
        if (sc_val_is_truthy(&cond))
            rc = sc_eval_node(ctx, node->u.if_expr.then_br, out);
        else if (node->u.if_expr.else_br)
            rc = sc_eval_node(ctx, node->u.if_expr.else_br, out);
        else
            sc_val_bool(out, 0);
        sc_val_free(&cond);
        break;
    }

    case SC_NODE_LIST: {
        ScVal *elems = NULL;
        ScVal *lead = NULL;
        size_t n = 0, cap = 0;
        int i;

        for (i = 0; i < node->u.list.count; i++) {
            ScVal ev = {0};
            rc = sc_eval_node(ctx, node->u.list.elems[i], &ev);
            if (rc != SC_OK) { sc_val_free(&ev); break; }
            if (n >= cap) {
                size_t nc = cap ? cap * 2 : SC_STACK_INIT_CAP;
                ScVal *ne = (ScVal *)realloc(elems, nc * sizeof(ScVal));
                if (!ne) { sc_val_free(&ev); rc = SC_ERR_NOMEM; break; }
                elems = ne;
                cap = nc;
            }
            elems[n++] = ev;
            if (n == 1)
                lead = &elems[0];
        }
        if (rc != SC_OK) {
            while (n) sc_val_free(&elems[--n]);
            free(elems);
            sc_val_err(out, 1);
            break;
        }
        if (lead && n > 1 && lead->type == SC_VAL_NUM) {
            ScVal anchor = {0};
            rc = sc_val_copy(&anchor, lead);
            if (rc == SC_OK) {
                sc_val_free(&elems[0]);
                elems[0] = anchor;
            }
        }
        sc_val_list(out, elems, n);
        free(elems);
        break;
    }

    case SC_NODE_PAREN:
        rc = sc_eval_node(ctx, node->u.unary.operand, out);
        break;

    default:
        sc_eval_set_error(ctx, SC_ERR_INTERNAL, "bad node %d", node->kind);
        sc_val_err(out, 1);
        rc = SC_ERR_INTERNAL;
        break;
    }

    ctx->depth--;
    return rc;
}

static void mark_formulas_dirty(ScSheet *sh)
{
    size_t i, n;
    if (!sh || !sh->cells) return;
    n = (size_t)sh->nrows * (size_t)sh->ncols;
    if (n > sh->cell_cap) n = sh->cell_cap;
    for (i = 0; i < n; i++)
        if (sh->cells[i].formula) sh->cells[i].dirty = 1;
    sh->flags &= ~SC_SHEET_CALC_DONE;
}

static int eval_one_sheet(ScWorkbook *wb, ScSheet *sh, int idx)
{
    ScEvalCtx *ctx = &wb->eval;
    size_t i, n;
    int rc = SC_OK;

    g_eval_sheet = idx;
    mark_formulas_dirty(sh);
    n = (size_t)sh->nrows * (size_t)sh->ncols;
    if (n > sh->cell_cap) n = sh->cell_cap;

    for (i = 0; i < n; i++) {
        ScCell *cl = &sh->cells[i];
        ScVal res = {0};

        if (!cl->formula || !cl->dirty) continue;
        rc = sc_eval_node(ctx, cl->formula->root, &res);
        if (rc != SC_OK) { sc_val_free(&res); if (ctx->had_error) break; continue; }
        sc_val_free(&cl->value);
        if (sc_val_copy(&cl->value, &res) != SC_OK) { sc_val_free(&res); rc = SC_ERR_NOMEM; break; }
        sc_val_free(&res);
        cl->dirty = 0;
    }

    if (rc == SC_OK && !ctx->had_error) sh->flags |= SC_SHEET_CALC_DONE;
    g_eval_sheet = -1;
    return ctx->had_error ? SC_ERR_INTERNAL : rc;
}

int sc_evaluate_sheet(sc_workbook *wb, sc_sheet *s)
{
    ScWorkbook *w = SC_WB(wb);
    ScSheet *sh = SC_SH(s);
    int idx, rc;

    if (!wb || !s) return SC_ERR_RANGE;
    for (idx = 0; idx < w->nsheets; idx++)
        if (&w->sheets[idx] == sh) break;
    if (idx >= w->nsheets) return SC_ERR_RANGE;

    rc = eval_ctx_prepare(w);
    if (rc != SC_OK) return rc;
    return eval_one_sheet(w, sh, idx);
}

int sc_evaluate(sc_workbook *wb)
{
    ScWorkbook *w = SC_WB(wb);
    int i, rc, last = SC_OK;

    if (!wb) return SC_ERR_RANGE;
    rc = eval_ctx_prepare(w);
    if (rc != SC_OK) return rc;

    for (i = 0; i < w->nsheets; i++) {
        rc = eval_one_sheet(w, &w->sheets[i], i);
        if (rc != SC_OK) last = rc;
        eval_clear_memo(&w->eval);
    }
    w->flags &= ~SC_WB_DIRTY;
    return last;
}
