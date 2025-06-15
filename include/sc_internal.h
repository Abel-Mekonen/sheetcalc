/*
 * sc_internal.h — private types & helpers for sheetcalc
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
 *
 * NOT part of the public API — do not include from user code.
 */

#ifndef SC_INTERNAL_H
#define SC_INTERNAL_H

#include "sheetcalc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* limits & tuning knobs                                              */
/* ------------------------------------------------------------------ */

#define SC_MAX_RECURSE      512
#define SC_MAX_FORMULA_LEN  8192
#define SC_MAX_SHEET_NAME   256
#define SC_BUF_INIT_CAP     64
#define SC_STACK_INIT_CAP   32
#define SC_SCOPE_INIT_CAP   8
#define SC_MEMO_INIT_CAP    64

#define SC_CELL_IDX(s, r, c)  ((size_t)(r) * (size_t)(s)->ncols + (size_t)(c))
#define SC_MEMO_KEY(s, r, c)  (((uint64_t)(s) << 32) | ((uint64_t)(r) << 16) | (uint64_t)(c))

/* sheet flags */
#define SC_SHEET_HIDDEN     0x01
#define SC_SHEET_PROTECTED  0x02
#define SC_SHEET_CALC_DONE  0x04

/* wb flags */
#define SC_WB_DIRTY         0x01
#define SC_WB_READONLY      0x02

/* ------------------------------------------------------------------ */
/* core value types                                                   */
/* ------------------------------------------------------------------ */

/* String builder — growable byte buffer */
typedef struct {
    char *data;
    size_t len, cap;
} ScBuf;

/* A single value in the evaluation engine */
typedef struct ScVal {
    int type;    /* SC_VAL_xxx */
    union {
        double num;
        struct { char *s; size_t len; } str;
        int boolean;
        int err;   /* error code */
        struct { struct ScVal *items; size_t count; } list;
    } as;
} ScVal;

/* AST node types for formulas */
enum {
    SC_NODE_LIT = 1,
    SC_NODE_REF,      /* cell reference A1, $B$2 */
    SC_NODE_RANGE,    /* range A1:B5 */
    SC_NODE_BINARY,   /* +, -, *, /, %, ^, &, =, <>, <, >, <=, >= */
    SC_NODE_UNARY,    /* -, + (prefix), NOT */
    SC_NODE_CALL,     /* function call SUM(...) */
    SC_NODE_NAME,     /* named range/constant */
    SC_NODE_IF,       /* IF(cond, then, else) */
    SC_NODE_LIST,     /* {1,2,3} array literal */
    SC_NODE_CONCAT,   /* string concatenation & */
    SC_NODE_PAREN,    /* parenthesized expr */
};

/* Binary operators */
enum {
    SC_OP_ADD = 1, SC_OP_SUB, SC_OP_MUL, SC_OP_DIV,
    SC_OP_MOD, SC_OP_POW, SC_OP_EQ, SC_OP_NE,
    SC_OP_LT, SC_OP_GT, SC_OP_LE, SC_OP_GE,
    SC_OP_AND, SC_OP_OR, SC_OP_CONCAT,
};

/* AST node */
typedef struct ScNode {
    int kind;
    union {
        ScVal lit;
        struct { int sheet; int row; int col; int abs_row; int abs_col; } ref;
        struct { int sheet; int r0, c0, r1, c1; } range;
        struct { int op; struct ScNode *lhs, *rhs; } binary;
        struct { int op; struct ScNode *operand; } unary;
        struct { int func_id; struct ScNode **args; int nargs; } call;
        struct { char *name; size_t name_len; } name;
        struct { struct ScNode *cond, *then_br, *else_br; } if_expr;
        struct { struct ScNode **elems; int count; } list;
    } u;
} ScNode;

/* Compiled formula */
typedef struct {
    ScNode *root;
    char *text;       /* original formula text */
    size_t text_len;
} ScFormula;

/* A cell */
typedef struct {
    ScVal value;
    ScFormula *formula;  /* NULL if literal */
    int dirty;
    uint32_t style_id;
} ScCell;

/* Named range definition */
typedef struct {
    char *name;
    int sheet;      /* -1 = workbook scope */
    int r0, c0, r1, c1;
} CsNameDef;

/* A sheet */
typedef struct {
    char *name;
    size_t name_len;
    ScCell *cells;
    int nrows, ncols;
    size_t cell_cap;
    uint32_t flags;
} ScSheet;

/* Style entry */
typedef struct {
    uint32_t fmt_id;
    uint32_t font_id;
    uint32_t fill_color;
    uint32_t border_mask;
    uint8_t align;
    uint8_t wrap;
} CsStyle;

/* Evaluation context — threaded through recursive eval */
typedef struct {
    ScVal *stack;       /* value stack */
    size_t stack_len, stack_cap;

    struct { char *name; ScVal val; } *scope;  /* named bindings */
    size_t scope_len, scope_cap;

    struct { uint64_t key; ScVal val; } *memo;  /* memoized results */
    size_t memo_len, memo_cap;

    ScBuf sb;           /* shared string builder */

    int depth;          /* recursion depth */
    int had_error;
    char error[256];

    struct ScWorkbook *wb;  /* backpointer */
} ScEvalCtx;

/* Workbook (top-level container) */
typedef struct ScWorkbook {
    ScSheet *sheets;
    int nsheets;
    size_t sheet_cap;

    CsNameDef *names;
    int nnames;
    size_t name_cap;

    CsStyle *styles;
    int nstyles;
    size_t style_cap;

    ScEvalCtx eval;    /* evaluation context */

    uint32_t version;
    uint32_t flags;
    uint8_t *raw;       /* raw file buffer */
    size_t raw_len;
} ScWorkbook;

// public opaque typedefs -> ScWorkbook et al. (see workbook.c)
#define SC_WB(w)  ((ScWorkbook *)(w))
#define SC_SH(s)  ((ScSheet *)(s))
#define SC_CL(c)  ((ScCell *)(c))

/* ------------------------------------------------------------------ */
/* ScBuf                                                              */
/* ------------------------------------------------------------------ */

void  sci_buf_init(ScBuf *b);
int   sci_buf_append(ScBuf *b, const char *s, size_t len);
int   sci_buf_appendf(ScBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void  sci_buf_reset(ScBuf *b);
void  sci_buf_free(ScBuf *b);
char *sci_buf_cstr(ScBuf *b);   /* NUL-term view; invalid after next append */

/* ------------------------------------------------------------------ */
/* ScVal                                                              */
/* ------------------------------------------------------------------ */

void sc_val_nil(ScVal *v);
void sc_val_num(ScVal *v, double n);
void sc_val_str(ScVal *v, const char *s);
void sc_val_str_n(ScVal *v, const char *s, size_t len);
void sc_val_bool(ScVal *v, int b);
void sc_val_err(ScVal *v, int code);
void sc_val_list(ScVal *v, ScVal *items, size_t count);

int  sc_val_copy(ScVal *dst, const ScVal *src);
void sc_val_free(ScVal *v);

int  sc_val_to_num(const ScVal *v, double *out);
int  sc_val_to_str(const ScVal *v, ScBuf *sb);
int  sc_val_is_truthy(const ScVal *v);
int  sc_val_cmp(const ScVal *a, const ScVal *b);  /* -1/0/+1, type errors -> 0 w/ err set? */

/* ------------------------------------------------------------------ */
/* eval                                                               */
/* ------------------------------------------------------------------ */

int  sc_eval_push(ScEvalCtx *ctx, const ScVal *v);
int  sc_eval_pop(ScEvalCtx *ctx, ScVal *out);
int  sc_eval_node(ScEvalCtx *ctx, ScNode *node, ScVal *out);
void sc_eval_set_error(ScEvalCtx *ctx, int code, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ------------------------------------------------------------------ */
/* parser                                                             */
/* ------------------------------------------------------------------ */

int sci_parse_workbook(const uint8_t *data, size_t len, ScWorkbook **out);
int sci_parse_formula_text(ScWorkbook *wb, int sheet,
                           const char *text, size_t text_len,
                           ScFormula **out);

/* ------------------------------------------------------------------ */
/* scope & memo                                                       */
/* ------------------------------------------------------------------ */

int  sci_scope_push(ScEvalCtx *ctx, const char *name, const ScVal *val);
void sci_scope_pop(ScEvalCtx *ctx, int n);
int  sci_scope_find(ScEvalCtx *ctx, const char *name, ScVal **out);

int  sci_memo_get(ScEvalCtx *ctx, uint64_t key, ScVal **out);
void sci_memo_put(ScEvalCtx *ctx, uint64_t key, const ScVal *val);

/* ------------------------------------------------------------------ */
/* builtins                                                           */
/* ------------------------------------------------------------------ */

int  sci_builtin_call(ScEvalCtx *ctx, int func_id, ScVal *args, int nargs, ScVal *out);
void sci_register_builtins(void);

/* ------------------------------------------------------------------ */
/* misc internals                                                     */
/* ------------------------------------------------------------------ */

int  sci_resolve_name(ScWorkbook *wb, const char *name, size_t name_len,
                      CsNameDef **out);
int  sci_cell_at(ScSheet *s, int row, int col, ScCell **out);
int  sci_both_num(ScEvalCtx *ctx, const ScVal *a, const ScVal *b,
                  double *out_a, double *out_b);
int  sci_format_cell(const ScCell *c, ScBuf *sb);

enum { SC_UOP_NEG = 1, SC_UOP_POS, SC_UOP_NOT };

/* func ids — full table lives in builtins.c */
/* ------------------------------------------------------------------ */
/* pivot tables                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int group_col;
    int value_col;
    int agg_type;       /* 0=sum, 1=count, 2=avg, 3=min, 4=max */
} CsPivotSpec;

typedef struct {
    char *label;
    double value;
    int count;
    double sum, min, max;
} CsPivotBucket;

typedef struct CsPivot CsPivot;

CsPivot *sci_pivot_create(ScSheet *sh, CsPivotSpec *specs, int nspecs,
                          int r0, int c0, int r1, int c1,
                          int row_field, int col_field);
void     sci_pivot_free(CsPivot *pv);
int      sci_pivot_add_row(CsPivot *pv, int row);
int      sci_pivot_finalize(CsPivot *pv);
int      sci_pivot_get_result(CsPivot *pv, ScVal **out, size_t *nrows, size_t *ncols);
int      sci_pivot_bucket_sort(CsPivotBucket *buckets, size_t count, int descending);
int      sci_pivot_match_label(const CsPivotBucket *b, const char *label);
void     sci_pivot_bucket_set_label(CsPivotBucket *b, const char *label);
double   sci_pivot_agg_value(const CsPivotBucket *b, int agg_type);

/* ------------------------------------------------------------------ */
/* sorting                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int col;
    int descending;     /* 0=asc, 1=desc */
    int numeric;        /* 0=string compare, 1=numeric */
} CsSortKey;

int sci_sort_range(ScSheet *sh, int r0, int c0, int r1, int c1,
                   CsSortKey *keys, int nkeys);
int sci_sort_vals(ScVal *vals, size_t count, CsSortKey *keys, int nkeys, int ncols);
int sci_sort_vals_inplace(ScVal *vals, size_t row_count, CsSortKey *keys,
                          int nkeys, int ncols);
int sci_sort_stable(ScVal *vals, size_t count, CsSortKey *keys, int nkeys, int ncols);
int sci_sort_compare_key(const ScVal *a, const ScVal *b, const CsSortKey *key);
int csi_rank_range(ScSheet *sh, int r0, int c0, int r1, int c1,
                   int key_col, int numeric, ScVal *ranks_out);
int csi_unique_range(ScSheet *sh, int r0, int c0, int r1, int c1, int *out_r1);

/* ------------------------------------------------------------------ */
/* statistics                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int count;
    double mean, median, mode;
    double variance, stdev;
    double min, max, sum;
    double skewness, kurtosis;
    double q1, q3, iqr;
} CsStatResult;

typedef struct {
    double lo, hi;
    int count;
} CsFreqBucket;

int    sci_stats_describe(ScVal *vals, size_t count, CsStatResult *out);
double sci_stats_percentile(ScVal *vals, size_t count, double pct);
double sci_stats_correlation(ScVal *xs, ScVal *ys, size_t count);
double sci_stats_covariance(ScVal *xs, ScVal *ys, size_t count, int sample);
int    sci_stats_regression(ScVal *xs, ScVal *ys, size_t count,
                            double *slope, double *intercept, double *r2);
int    sci_stats_frequency(ScVal *vals, size_t count, int nbuckets, CsFreqBucket *out);
double sci_stats_mean(const ScVal *vals, size_t count);
double sci_stats_stdev(const ScVal *vals, size_t count, int sample);
int    sci_stats_quartiles(ScVal *vals, size_t count, double *q1, double *q2, double *q3);
int    sci_stats_range_from_sheet(ScSheet *sh, int r0, int c0, int r1, int c1,
                                  CsStatResult *out);
int    sci_stats_zscore(ScVal *vals, size_t count, double *zout);
int    sci_stats_moving_average(const ScVal *vals, size_t count, int window, double *out);
double sci_stats_geometric_mean(ScVal *vals, size_t count);
double sci_stats_harmonic_mean(ScVal *vals, size_t count);

enum {
    SC_FN_SUM = 1, SC_FN_AVG, SC_FN_MIN, SC_FN_MAX, SC_FN_COUNT,
    SC_FN_IF, SC_FN_AND, SC_FN_OR, SC_FN_NOT, SC_FN_ABS,
    SC_FN_ROUND, SC_FN_CONCAT, SC_FN_LEN,
    SC_FN_ISBLANK, SC_FN_ISERROR, SC_FN_ISNA, SC_FN_NA,
    SC_FN_VLOOKUP, SC_FN_HLOOKUP, SC_FN_INDEX, SC_FN_MATCH,
    SC_FN_TEXT, SC_FN_VALUE, SC_FN_TODAY, SC_FN_NOW,
    SC_FN_LEFT, SC_FN_RIGHT, SC_FN_MID, SC_FN_FIND,
    SC_FN_UPPER, SC_FN_LOWER, SC_FN_TRIM,
    SC_FN_PI, SC_FN_SQRT, SC_FN_LOG, SC_FN_LN, SC_FN_EXP,
    SC_FN_MOD_FUNC,   /* MOD() — clashes w/ SC_OP_MOD */
    SC_FN__COUNT,     /* sentinel */
};

#ifdef __cplusplus
}
#endif

#endif /* SC_INTERNAL_H */
