/*
 * test_eval.c — formula evaluation tests
 */

#include "../include/sc_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(const char *name, int ok)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

typedef struct {
    int row;
    int col;
    int is_formula;
    union {
        double num;
        const char *text;
    } u;
} CellSpec;

static void free_wb(ScWorkbook *wb);

static ScWorkbook *make_test_wb(int rows, int cols, const CellSpec *specs, int nspec)
{
    ScWorkbook *wb;
    ScSheet *sh;
    int i;

    wb = (ScWorkbook *)calloc(1, sizeof(*wb));
    if (!wb)
        return NULL;

    wb->nsheets = 1;
    wb->sheets = (ScSheet *)calloc(1, sizeof(ScSheet));
    if (!wb->sheets) {
        free(wb);
        return NULL;
    }

    sh = &wb->sheets[0];
    sh->name = (char *)malloc(7);
    if (sh->name)
        strcpy(sh->name, "Sheet1");
    sh->name_len = 6;
    sh->nrows = rows;
    sh->ncols = cols;
    sh->cell_cap = (size_t)rows * (size_t)cols;
    sh->cells = (ScCell *)calloc(sh->cell_cap, sizeof(ScCell));
    if (!sh->cells) {
        free(sh->name);
        free(wb->sheets);
        free(wb);
        return NULL;
    }

    sci_buf_init(&wb->eval.sb);
    wb->eval.wb = wb;

    for (i = 0; i < nspec; i++) {
        ScCell *cl;
        const CellSpec *sp = &specs[i];

        if (sci_cell_at(sh, sp->row, sp->col, &cl) != SC_OK)
            continue;

        if (sp->is_formula) {
            ScFormula *fml = NULL;
            const char *txt = sp->u.text;
            size_t tlen = strlen(txt);

            if (sci_parse_formula_text(wb, 0, txt, tlen, &fml) != SC_OK) {
                free_wb(wb);
                return NULL;
            }
            cl->formula = fml;
            cl->dirty = 1;
        } else {
            sc_val_num(&cl->value, sp->u.num);
        }
    }

    return wb;
}

static void free_wb(ScWorkbook *wb)
{
    size_t i, n;
    ScSheet *sh;

    if (!wb)
        return;
    sh = wb->sheets;
    if (sh && sh->cells) {
        n = (size_t)sh->nrows * (size_t)sh->ncols;
        if (n > sh->cell_cap)
            n = sh->cell_cap;
        for (i = 0; i < n; i++) {
            sc_val_free(&sh->cells[i].value);
            if (sh->cells[i].formula) {
                free(sh->cells[i].formula->text);
                free(sh->cells[i].formula);
            }
        }
        free(sh->cells);
    }
    if (sh)
        free(sh->name);
    free(wb->sheets);
    sci_buf_free(&wb->eval.sb);
    free(wb);
}

static int eval_cell(ScWorkbook *wb, int row, int col, ScVal *out)
{
    ScCell *cl;

    if (sci_cell_at(&wb->sheets[0], row, col, &cl) != SC_OK)
        return SC_ERR_RANGE;
    if (sc_evaluate((sc_workbook *)wb) != SC_OK)
        return SC_ERR_INTERNAL;
    return sc_val_copy(out, &cl->value);
}

static int test_simple_arithmetic(void)
{
    CellSpec specs[] = {
        {0, 0, 1, {.text = "=1+2"}},
    };
    ScWorkbook *wb = make_test_wb(1, 1, specs, 1);
    ScVal v;
    int ok = 0;

    sc_val_nil(&v);
    if (wb && eval_cell(wb, 0, 0, &v) == SC_OK)
        ok = (v.type == SC_VAL_NUM && fabs(v.as.num - 3.0) < 1e-9);
    sc_val_free(&v);
    free_wb(wb);
    return ok ? 0 : 1;
}

static int test_cell_reference(void)
{
    CellSpec specs[] = {
        {0, 0, 0, {.num = 5.0}},
        {1, 0, 1, {.text = "=A1*2"}},
    };
    ScWorkbook *wb = make_test_wb(2, 1, specs, 2);
    ScVal v;
    int ok = 0;

    sc_val_nil(&v);
    if (wb && eval_cell(wb, 1, 0, &v) == SC_OK)
        ok = (v.type == SC_VAL_NUM && fabs(v.as.num - 10.0) < 1e-9);
    sc_val_free(&v);
    free_wb(wb);
    return ok ? 0 : 1;
}

static int test_sum_function(void)
{
    CellSpec specs[] = {
        {0, 0, 0, {.num = 1.0}},
        {1, 0, 0, {.num = 2.0}},
        {2, 0, 0, {.num = 3.0}},
        {3, 0, 1, {.text = "=SUM(A1:A3)"}},
    };
    ScWorkbook *wb = make_test_wb(4, 1, specs, 4);
    ScVal v;
    int ok = 0;

    sc_val_nil(&v);
    if (wb && eval_cell(wb, 3, 0, &v) == SC_OK)
        ok = (v.type == SC_VAL_NUM && fabs(v.as.num - 6.0) < 1e-9);
    sc_val_free(&v);
    free_wb(wb);
    return ok ? 0 : 1;
}

static int test_string_concat(void)
{
    CellSpec specs[] = {
        {0, 0, 1, {.text = "=CONCATENATE(\"hello\",\" \",\"world\")"}},
    };
    ScWorkbook *wb = make_test_wb(1, 1, specs, 1);
    ScVal v;
    int ok = 0;

    sc_val_nil(&v);
    if (wb && eval_cell(wb, 0, 0, &v) == SC_OK)
        ok = (v.type == SC_VAL_STR && v.as.str.len == 11
              && memcmp(v.as.str.s, "hello world", 11) == 0);
    sc_val_free(&v);
    free_wb(wb);
    return ok ? 0 : 1;
}

static int test_boolean_and(void)
{
    CellSpec specs[] = {
        {0, 0, 1, {.text = "=AND(TRUE,FALSE)"}},
    };
    ScWorkbook *wb = make_test_wb(1, 1, specs, 1);
    ScVal v;
    int ok = 0;

    sc_val_nil(&v);
    if (wb && eval_cell(wb, 0, 0, &v) == SC_OK)
        ok = (v.type == SC_VAL_BOOL && v.as.boolean == 0);
    sc_val_free(&v);
    free_wb(wb);
    return ok ? 0 : 1;
}

typedef int (*test_fn)(void);

static const struct {
    const char *name;
    test_fn fn;
} tests[] = {
    {"simple_arithmetic", test_simple_arithmetic},
    {"cell_reference",    test_cell_reference},
    {"sum_function",      test_sum_function},
    {"string_concat",     test_string_concat},
    {"boolean_and",       test_boolean_and},
};

int main(void)
{
    size_t i;

    printf("test_eval (%zu cases)\n", sizeof(tests)/sizeof(tests[0]));
    for (i = 0; i < sizeof(tests)/sizeof(tests[0]); i++)
        check(tests[i].name, tests[i].fn() == 0);
    printf(failures ? "\n%d failure(s)\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
