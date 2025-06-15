/* test_formula.c — formula parser tests */

#include "../include/sc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(const char *name, int ok)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

static ScWorkbook *make_wb(void)
{
    ScWorkbook *wb = calloc(1, sizeof(*wb));
    ScSheet *sh;
    if (!wb) return NULL;
    wb->nsheets = 1;
    wb->sheets = calloc(1, sizeof(ScSheet));
    if (!wb->sheets) { free(wb); return NULL; }
    sh = &wb->sheets[0];
    sh->name = strdup("Sheet1");
    sh->name_len = 6;
    sh->nrows = 20;
    sh->ncols = 10;
    sh->cell_cap = (size_t)sh->nrows * (size_t)sh->ncols;
    sh->cells = calloc(sh->cell_cap, sizeof(ScCell));
    sci_buf_init(&wb->eval.sb);
    wb->eval.wb = wb;
    return wb;
}

static void done(ScWorkbook *wb, ScFormula *f)
{
    if (f) { free(f->text); free(f); }
    if (!wb) return;
    free(wb->sheets[0].name);
    free(wb->sheets[0].cells);
    free(wb->sheets);
    sci_buf_free(&wb->eval.sb);
    free(wb);
}

static int parse(ScWorkbook *wb, const char *txt, ScFormula **f)
{
    return sci_parse_formula_text(wb, 0, txt, strlen(txt), f);
}

static int test_simple_number(void)
{
    ScWorkbook *wb = make_wb();
    ScFormula *f = NULL;
    ScNode *n;
    int ok = 0;
    if (wb && parse(wb, "=42", &f) == SC_OK && f && (n = f->root))
        ok = n->kind == SC_NODE_LIT && n->u.lit.type == SC_VAL_NUM && n->u.lit.as.num == 42.0;
    done(wb, f);
    return ok ? 0 : 1;
}

static int test_simple_ref(void)
{
    ScWorkbook *wb = make_wb();
    ScFormula *f = NULL;
    int ok = 0;
    if (wb && parse(wb, "=A1", &f) == SC_OK && f && f->root)
        ok = f->root->kind == SC_NODE_REF && f->root->u.ref.row == 0 && f->root->u.ref.col == 0;
    done(wb, f);
    return ok ? 0 : 1;
}

static int test_binary_op(void)
{
    ScWorkbook *wb = make_wb();
    ScFormula *f = NULL;
    ScNode *n;
    int ok = 0;
    if (wb && parse(wb, "=A1+B2", &f) == SC_OK && f && (n = f->root))
        ok = n->kind == SC_NODE_BINARY && n->u.binary.op == SC_OP_ADD
             && n->u.binary.lhs->kind == SC_NODE_REF && n->u.binary.rhs->kind == SC_NODE_REF;
    done(wb, f);
    return ok ? 0 : 1;
}

static int test_function_call(void)
{
    ScWorkbook *wb = make_wb();
    ScFormula *f = NULL;
    ScNode *n;
    int ok = 0;
    if (wb && parse(wb, "=SUM(A1:A5)", &f) == SC_OK && f && (n = f->root))
        ok = n->kind == SC_NODE_CALL && n->u.call.func_id == SC_FN_SUM
             && n->u.call.nargs == 1 && n->u.call.args[0]->kind == SC_NODE_RANGE;
    done(wb, f);
    return ok ? 0 : 1;
}

static int test_nested_if(void)
{
    ScWorkbook *wb = make_wb();
    ScFormula *f = NULL;
    int ok = 0;
    if (wb && parse(wb, "=IF(A1>0,SUM(A1:A5),0)", &f) == SC_OK && f && f->root)
        ok = f->root->kind == SC_NODE_CALL && f->root->u.call.func_id == SC_FN_IF
             && f->root->u.call.nargs == 3;
    done(wb, f);
    return ok ? 0 : 1;
}

static int test_empty_string(void)
{
    ScWorkbook *wb = make_wb();
    ScFormula *f = NULL;
    int rc = wb ? sci_parse_formula_text(wb, 0, "", 0, &f) : SC_ERR_NOMEM;
    done(wb, f);
    return rc == SC_ERR_FORMAT ? 0 : 1;
}

static int test_unbalanced_parens(void)
{
    ScWorkbook *wb = make_wb();
    ScFormula *f = NULL;
    int rc = wb ? parse(wb, "=SUM(A1", &f) : SC_ERR_NOMEM;
    done(wb, f);
    return rc == SC_ERR_FORMAT ? 0 : 1;
}

typedef int (*test_fn)(void);

static const struct { const char *name; test_fn fn; } tests[] = {
    {"simple_number", test_simple_number},
    {"simple_ref", test_simple_ref},
    {"binary_op", test_binary_op},
    {"function_call", test_function_call},
    {"nested_if", test_nested_if},
    {"empty_string", test_empty_string},
    {"unbalanced_parens", test_unbalanced_parens},
};

int main(void)
{
    size_t i;
    printf("test_formula (%zu cases)\n", sizeof(tests)/sizeof(tests[0]));
    for (i = 0; i < sizeof(tests)/sizeof(tests[0]); i++)
        check(tests[i].name, tests[i].fn() == 0);
    printf(failures ? "\n%d failure(s)\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
