/*
 * formula.c — formula text → AST parser for sheetcalc
 *
 * Copyright (c) 2024-2025 Sheetcalc Contributors
 * SPDX-License-Identifier: MIT
 */

#include "../include/sc_internal.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *text;
    size_t len;
    size_t pos;
    ScWorkbook *wb;
    int sheet;
} FmlParser;

static const struct { const char *name; int id; } fn_table[] = {
    {"SUM", SC_FN_SUM}, {"AVG", SC_FN_AVG}, {"AVERAGE", SC_FN_AVG},
    {"MIN", SC_FN_MIN}, {"MAX", SC_FN_MAX}, {"COUNT", SC_FN_COUNT},
    {"IF", SC_FN_IF}, {"AND", SC_FN_AND}, {"OR", SC_FN_OR}, {"NOT", SC_FN_NOT},
    {"ABS", SC_FN_ABS}, {"ROUND", SC_FN_ROUND}, {"SQRT", SC_FN_SQRT},
    {"LOG", SC_FN_LOG}, {"LN", SC_FN_LN}, {"EXP", SC_FN_EXP}, {"PI", SC_FN_PI},
    {"MOD", SC_FN_MOD_FUNC},
    {"CONCATENATE", SC_FN_CONCAT}, {"CONCAT", SC_FN_CONCAT}, {"LEN", SC_FN_LEN},
    {"LEFT", SC_FN_LEFT}, {"RIGHT", SC_FN_RIGHT}, {"MID", SC_FN_MID},
    {"FIND", SC_FN_FIND}, {"UPPER", SC_FN_UPPER}, {"LOWER", SC_FN_LOWER},
    {"TRIM", SC_FN_TRIM},
    {"ISBLANK", SC_FN_ISBLANK}, {"ISERROR", SC_FN_ISERROR}, {"ISNA", SC_FN_ISNA},
    {"NA", SC_FN_NA},
    {"VLOOKUP", SC_FN_VLOOKUP}, {"INDEX", SC_FN_INDEX}, {"MATCH", SC_FN_MATCH},
    {"TEXT", SC_FN_TEXT}, {"VALUE", SC_FN_VALUE},
    {"TODAY", SC_FN_TODAY}, {"NOW", SC_FN_NOW},
};

static ScNode *make_node(int kind)
{
    ScNode *n = (ScNode *)calloc(1, sizeof(*n));
    if (n) n->kind = kind;
    return n;
}

static void free_node(ScNode *node)
{
    int i;
    if (!node) return;
    switch (node->kind) {
    case SC_NODE_LIT: sc_val_free(&node->u.lit); break;
    case SC_NODE_BINARY:
    case SC_NODE_CONCAT:
        free_node(node->u.binary.lhs);
        free_node(node->u.binary.rhs);
        break;
    case SC_NODE_UNARY:
    case SC_NODE_PAREN: free_node(node->u.unary.operand); break;
    case SC_NODE_CALL:
        for (i = 0; i < node->u.call.nargs; i++) free_node(node->u.call.args[i]);
        free(node->u.call.args);
        break;
    case SC_NODE_NAME: free(node->u.name.name); break;
    case SC_NODE_IF:
        free_node(node->u.if_expr.cond);
        free_node(node->u.if_expr.then_br);
        free_node(node->u.if_expr.else_br);
        break;
    case SC_NODE_LIST:
        for (i = 0; i < node->u.list.count; i++) free_node(node->u.list.elems[i]);
        free(node->u.list.elems);
        break;
    default: break;
    }
    free(node);
}

static char peek(FmlParser *fp)
{
    return (fp->pos < fp->len) ? fp->text[fp->pos] : '\0';
}

static char advance(FmlParser *fp)
{
    char c = peek(fp);
    if (fp->pos < fp->len) fp->pos++;
    return c;
}

static void skip_ws(FmlParser *fp)
{
    while (fp->pos < fp->len && isspace((unsigned char)fp->text[fp->pos]))
        fp->pos++;
}

static int match(FmlParser *fp, char c)
{
    skip_ws(fp);
    if (peek(fp) != c) return 0;
    advance(fp);
    return 1;
}

static char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static int ieq_n(const char *a, size_t alen, const char *b)
{
    size_t i, bl = strlen(b);
    if (alen != bl) return 0;
    for (i = 0; i < alen; i++)
        if (up(a[i]) != up(b[i])) return 0;
    return 1;
}

static int lookup_fn(const char *nm, size_t nlen, int *id)
{
    size_t i;
    for (i = 0; i < sizeof(fn_table)/sizeof(fn_table[0]); i++)
        if (ieq_n(nm, nlen, fn_table[i].name)) { *id = fn_table[i].id; return SC_OK; }
    return SC_ERR_FORMAT;
}

static int col_num(const char *s, size_t n, int *out)
{
    int v = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        char c = up(s[i]);
        if (c < 'A' || c > 'Z') return SC_ERR_FORMAT;
        v = v * 26 + (c - 'A' + 1);
    }
    if (v <= 0) return SC_ERR_FORMAT;
    *out = v - 1;
    return SC_OK;
}

static int row_num(const char *s, size_t n, int *out)
{
    long v = 0;
    size_t i;
    if (!n) return SC_ERR_FORMAT;
    for (i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) return SC_ERR_FORMAT;
        v = v * 10 + (s[i] - '0');
        if (v > 1048576L) return SC_ERR_OVERFLOW;
    }
    if (v <= 0) return SC_ERR_FORMAT;
    *out = (int)(v - 1);
    return SC_OK;
}

static int sheet_by_name(FmlParser *fp, const char *nm, size_t nlen, int *sh)
{
    int i;
    for (i = 0; i < fp->wb->nsheets; i++) {
        ScSheet *s = &fp->wb->sheets[i];
        if (s->name_len == nlen && !memcmp(s->name, nm, nlen)) {
            *sh = i;
            return SC_OK;
        }
    }
    return SC_ERR_FORMAT;
}

static int parse_expr(FmlParser *fp, ScNode **out);
static int parse_primary(FmlParser *fp, ScNode **out);
static int parse_cell_ref(FmlParser *fp, ScNode **out);
static int parse_range(FmlParser *fp, ScNode *first, ScNode **out);
static int parse_function_call(FmlParser *fp, const char *nm, size_t nlen, ScNode **out);
static int parse_name(FmlParser *fp, const char *nm, size_t nlen, ScNode **out);

static int parse_number(FmlParser *fp, ScNode **out)
{
    size_t st = fp->pos;
    int dot = 0, dig = 0;
    ScNode *node;
    char buf[64], *end;
    double v;

    if (peek(fp) == '.') {
        if (fp->pos + 1 >= fp->len || !isdigit((unsigned char)fp->text[fp->pos + 1]))
            return SC_ERR_FORMAT;
        dot = 1;
        advance(fp);
    }
    while (fp->pos < fp->len) {
        char c = fp->text[fp->pos];
        if (isdigit((unsigned char)c)) { dig = 1; advance(fp); }
        else if (c == '.' && !dot) { dot = 1; advance(fp); }
        else break;
    }
    if (!dig) return SC_ERR_FORMAT;

    node = make_node(SC_NODE_LIT);
    if (!node) return SC_ERR_NOMEM;
    if (fp->pos - st >= sizeof(buf)) { free_node(node); return SC_ERR_FORMAT; }
    memcpy(buf, fp->text + st, fp->pos - st);
    buf[fp->pos - st] = '\0';
    v = strtod(buf, &end);
    if (end != buf + (fp->pos - st)) { free_node(node); return SC_ERR_FORMAT; }
    sc_val_num(&node->u.lit, v);
    *out = node;
    return SC_OK;
}

static int parse_string(FmlParser *fp, ScNode **out)
{
    ScNode *node;
    ScBuf sb;
    char c;

    if (!match(fp, '"')) return SC_ERR_FORMAT;
    sci_buf_init(&sb);
    while (fp->pos < fp->len) {
        c = advance(fp);
        if (c == '"') {
            if (peek(fp) == '"') {
                if (sci_buf_append(&sb, "\"", 1) != SC_OK) { sci_buf_free(&sb); return SC_ERR_NOMEM; }
                advance(fp);
            } else break;
        } else if (sci_buf_append(&sb, &c, 1) != SC_OK) {
            sci_buf_free(&sb);
            return SC_ERR_NOMEM;
        }
    }
    node = make_node(SC_NODE_LIT);
    if (!node) { sci_buf_free(&sb); return SC_ERR_NOMEM; }
    sc_val_str_n(&node->u.lit, sb.data ? sb.data : "", sb.len);
    sci_buf_free(&sb);
    *out = node;
    return SC_OK;
}

static int parse_a1_tail(FmlParser *fp, int sh, ScNode *node)
{
    size_t c0, c1, r0, r1;
    int rc;
    if (match(fp, '$')) node->u.ref.abs_col = 1;
    c0 = fp->pos;
    while (fp->pos < fp->len && up(fp->text[fp->pos]) >= 'A' && up(fp->text[fp->pos]) <= 'Z') fp->pos++;
    c1 = fp->pos;
    if (c1 == c0) return SC_ERR_FORMAT;
    if (match(fp, '$')) node->u.ref.abs_row = 1;
    r0 = fp->pos;
    while (fp->pos < fp->len && isdigit((unsigned char)fp->text[fp->pos])) fp->pos++;
    r1 = fp->pos;
    if (r1 == r0) return SC_ERR_FORMAT;
    node->u.ref.sheet = sh;
    if ((rc = col_num(fp->text + c0, c1 - c0, &node->u.ref.col)) != SC_OK) return rc;
    return row_num(fp->text + r0, r1 - r0, &node->u.ref.row);
}

static int parse_cell_ref(FmlParser *fp, ScNode **out)
{
    size_t id0, id1;
    ScNode *node;
    ScBuf sb;
    int sh = fp->sheet, rc;

    skip_ws(fp);
    if (peek(fp) == '\'') {
        char c;
        if (!match(fp, '\'')) return SC_ERR_FORMAT;
        sci_buf_init(&sb);
        while (fp->pos < fp->len) {
            c = advance(fp);
            if (c == '\'' && peek(fp) == '\'') {
                if (sci_buf_append(&sb, "'", 1) != SC_OK) { sci_buf_free(&sb); return SC_ERR_NOMEM; }
                advance(fp);
            } else if (c == '\'') break;
            else if (sci_buf_append(&sb, &c, 1) != SC_OK) { sci_buf_free(&sb); return SC_ERR_NOMEM; }
        }
        if (!sb.len) { sci_buf_free(&sb); return SC_ERR_FORMAT; }
        rc = sheet_by_name(fp, sb.data, sb.len, &sh);
        sci_buf_free(&sb);
        if (rc != SC_OK || !match(fp, '!')) return SC_ERR_FORMAT;
    } else if (isalpha((unsigned char)peek(fp))) {
        id0 = fp->pos;
        while (fp->pos < fp->len) {
            char ch = fp->text[fp->pos];
            if (isalnum((unsigned char)ch) || ch == '_') fp->pos++;
            else break;
        }
        id1 = fp->pos;
        if (id1 > id0 && peek(fp) == '!') {
            rc = sheet_by_name(fp, fp->text + id0, id1 - id0, &sh);
            if (rc != SC_OK) return rc;
            advance(fp);
        } else {
            fp->pos = id0;
        }
    }

    node = make_node(SC_NODE_REF);
    if (!node) return SC_ERR_NOMEM;
    rc = parse_a1_tail(fp, sh, node);
    if (rc != SC_OK) { free_node(node); return rc; }
    *out = node;
    return SC_OK;
}

static int parse_range(FmlParser *fp, ScNode *first, ScNode **out)
{
    ScNode *node, *second;
    int rc;

    if (!first || first->kind != SC_NODE_REF || !match(fp, ':')) return SC_ERR_FORMAT;
    rc = parse_cell_ref(fp, &second);
    if (rc != SC_OK) { free_node(first); return rc; }
    if (second->kind != SC_NODE_REF) {
        free_node(first); free_node(second);
        return SC_ERR_FORMAT;
    }
    node = make_node(SC_NODE_RANGE);
    if (!node) { free_node(first); free_node(second); return SC_ERR_NOMEM; }
    node->u.range.sheet = first->u.ref.sheet;
    node->u.range.r0 = first->u.ref.row;
    node->u.range.c0 = first->u.ref.col;
    node->u.range.r1 = second->u.ref.row;
    node->u.range.c1 = second->u.ref.col;
    free_node(first);
    free_node(second);
    *out = node;
    return SC_OK;
}

static int push_arg(ScNode ***arr, int *n, int *cap, ScNode *el)
{
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 4;
        ScNode **na = (ScNode **)realloc(*arr, (size_t)nc * sizeof(ScNode *));
        if (!na) { free_node(el); return SC_ERR_NOMEM; }
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*n)++] = el;
    return SC_OK;
}

static int parse_comma_exprs(FmlParser *fp, char close, ScNode ***elems, int *n, int *cap)
{
    int rc;
    skip_ws(fp);
    if (peek(fp) == close) return SC_OK;
    for (;;) {
        ScNode *el;
        rc = parse_expr(fp, &el);
        if (rc != SC_OK) return rc;
        rc = push_arg(elems, n, cap, el);
        if (rc != SC_OK) return rc;
        skip_ws(fp);
        if (peek(fp) == close) break;
        if (!match(fp, ',')) return SC_ERR_FORMAT;
    }
    return SC_OK;
}

static int finish_ref(FmlParser *fp, ScNode *ref, ScNode **out)
{
    skip_ws(fp);
    return (peek(fp) == ':') ? parse_range(fp, ref, out) : (*out = ref, SC_OK);
}

static int try_ref(FmlParser *fp, ScNode **out)
{
    ScNode *ref;
    int rc = parse_cell_ref(fp, &ref);
    return (rc != SC_OK) ? rc : finish_ref(fp, ref, out);
}

static int parse_function_call(FmlParser *fp, const char *nm, size_t nlen, ScNode **out)
{
    ScNode **args = NULL, *node;
    int fn_id, rc, n = 0, cap = 0;

    if (lookup_fn(nm, nlen, &fn_id) != SC_OK || !match(fp, '(')) return SC_ERR_FORMAT;
    rc = parse_comma_exprs(fp, ')', &args, &n, &cap);
    if (rc != SC_OK) goto fail;
    if (!match(fp, ')')) { rc = SC_ERR_FORMAT; goto fail; }

    node = make_node(SC_NODE_CALL);
    if (!node) { rc = SC_ERR_NOMEM; goto fail; }
    node->u.call.func_id = fn_id;
    node->u.call.args = args;
    node->u.call.nargs = n;
    *out = node;
    return SC_OK;
fail:
    while (n > 0) free_node(args[--n]);
    free(args);
    return rc;
}

static int parse_name(FmlParser *fp, const char *nm, size_t nlen, ScNode **out)
{
    ScNode *node;
    char *cp;
    (void)fp;
    node = make_node(SC_NODE_NAME);
    if (!node) return SC_ERR_NOMEM;
    cp = (char *)malloc(nlen + 1);
    if (!cp) { free_node(node); return SC_ERR_NOMEM; }
    memcpy(cp, nm, nlen);
    cp[nlen] = '\0';
    node->u.name.name = cp;
    node->u.name.name_len = nlen;
    *out = node;
    return SC_OK;
}

static int parse_list(FmlParser *fp, ScNode **out)
{
    ScNode **elems = NULL, *node;
    int n = 0, cap = 0, rc;

    if (!match(fp, '{')) return SC_ERR_FORMAT;
    rc = parse_comma_exprs(fp, '}', &elems, &n, &cap);
    if (rc != SC_OK) goto fail;
    if (!match(fp, '}')) { rc = SC_ERR_FORMAT; goto fail; }
    node = make_node(SC_NODE_LIST);
    if (!node) { rc = SC_ERR_NOMEM; goto fail; }
    node->u.list.elems = elems;
    node->u.list.count = n;
    *out = node;
    return SC_OK;
fail:
    while (n > 0) free_node(elems[--n]);
    free(elems);
    return rc;
}

static int is_a1_token(FmlParser *fp, size_t st, size_t en)
{
    size_t i = st, d = en;
    if (st < en && fp->text[st] == '$') i++;
    while (i < en && up(fp->text[i]) >= 'A' && up(fp->text[i]) <= 'Z') i++;
    if (i < en && fp->text[i] == '$') i++;
    while (d > i && isdigit((unsigned char)fp->text[d - 1])) d--;
    return i < en && d == i;
}

static int parse_ident_or_ref(FmlParser *fp, ScNode **out)
{
    size_t st, en;
    ScNode *node;

    st = fp->pos;
    if (peek(fp) == '$') return try_ref(fp, out);

    if (!isalpha((unsigned char)peek(fp))) return SC_ERR_FORMAT;
    while (fp->pos < fp->len) {
        char c = fp->text[fp->pos];
        if (isalnum((unsigned char)c) || c == '_') fp->pos++;
        else break;
    }
    en = fp->pos;
    skip_ws(fp);

    if (peek(fp) == '(')
        return parse_function_call(fp, fp->text + st, en - st, out);
    if (ieq_n(fp->text + st, en - st, "TRUE") || ieq_n(fp->text + st, en - st, "FALSE")) {
        node = make_node(SC_NODE_LIT);
        if (!node) return SC_ERR_NOMEM;
        sc_val_bool(&node->u.lit, ieq_n(fp->text + st, en - st, "TRUE"));
        *out = node;
        return SC_OK;
    }
    if (peek(fp) == '!' || is_a1_token(fp, st, en)) {
        fp->pos = st;
        return try_ref(fp, out);
    }
    return parse_name(fp, fp->text + st, en - st, out);
}

static int parse_primary(FmlParser *fp, ScNode **out)
{
    ScNode *inner, *wrap;
    int rc;
    char ch;

    skip_ws(fp);
    ch = peek(fp);
    if (!ch) return SC_ERR_FORMAT;
    if (ch == '"') return parse_string(fp, out);
    if (ch == '{') return parse_list(fp, out);
    if (ch == '$' || isalpha((unsigned char)ch)) return parse_ident_or_ref(fp, out);
    if (isdigit((unsigned char)ch) || ch == '.') return parse_number(fp, out);

    if (ch != '(') return SC_ERR_FORMAT;
    advance(fp);
    rc = parse_expr(fp, &inner);
    if (rc != SC_OK) return rc;
    if (!match(fp, ')')) { free_node(inner); return SC_ERR_FORMAT; }
    wrap = make_node(SC_NODE_PAREN);
    if (!wrap) { free_node(inner); return SC_ERR_NOMEM; }
    wrap->u.unary.operand = inner;
    *out = wrap;
    return SC_OK;
}

static int parse_postfix(FmlParser *fp, ScNode **out)
{
    return parse_primary(fp, out);
}

static int parse_unary(FmlParser *fp, ScNode **out)
{
    ScNode *opnd, *node;
    int uop = 0;
    size_t st;

    skip_ws(fp);
    if (peek(fp) == '-') { uop = SC_UOP_NEG; advance(fp); }
    else if (peek(fp) == '+') { uop = SC_UOP_POS; advance(fp); }
    else if (isalpha((unsigned char)peek(fp))) {
        st = fp->pos;
        while (fp->pos < fp->len && isalpha((unsigned char)fp->text[fp->pos])) fp->pos++;
        if (ieq_n(fp->text + st, fp->pos - st, "NOT") && peek(fp) != '(')
            uop = SC_UOP_NOT;
        else
            fp->pos = st;
    }

    if (uop) {
        int rc = parse_unary(fp, &opnd);
        if (rc != SC_OK) return rc;
        node = make_node(SC_NODE_UNARY);
        if (!node) { free_node(opnd); return SC_ERR_NOMEM; }
        node->u.unary.op = uop;
        node->u.unary.operand = opnd;
        *out = node;
        return SC_OK;
    }
    return parse_postfix(fp, out);
}

static int parse_power(FmlParser *fp, ScNode **out)
{
    ScNode *base, *exp, *node;
    int rc = parse_unary(fp, &base);
    if (rc != SC_OK) return rc;
    skip_ws(fp);
    if (peek(fp) != '^') { *out = base; return SC_OK; }
    advance(fp);
    rc = parse_power(fp, &exp);
    if (rc != SC_OK) { free_node(base); return rc; }
    node = make_node(SC_NODE_BINARY);
    if (!node) { free_node(base); free_node(exp); return SC_ERR_NOMEM; }
    node->u.binary.op = SC_OP_POW;
    node->u.binary.lhs = base;
    node->u.binary.rhs = exp;
    *out = node;
    return SC_OK;
}

static int next_bin_op(FmlParser *fp, int tier)
{
    char c = peek(fp);
    if (tier == 0) {
        if (c == '*') { advance(fp); return SC_OP_MUL; }
        if (c == '/') { advance(fp); return SC_OP_DIV; }
        if (c == '%') { advance(fp); return SC_OP_MOD; }
    } else if (tier == 1) {
        if (c == '+') { advance(fp); return SC_OP_ADD; }
        if (c == '-') { advance(fp); return SC_OP_SUB; }
        if (c == '&') { advance(fp); return SC_OP_CONCAT; }
    } else {
        if (c == '=') { advance(fp); return SC_OP_EQ; }
        if (c == '<') {
            advance(fp);
            if (peek(fp) == '=') { advance(fp); return SC_OP_LE; }
            if (peek(fp) == '>') { advance(fp); return SC_OP_NE; }
            return SC_OP_LT;
        }
        if (c == '>') {
            advance(fp);
            if (peek(fp) == '=') { advance(fp); return SC_OP_GE; }
            return SC_OP_GT;
        }
    }
    return 0;
}

static int parse_bin(FmlParser *fp, ScNode **out,
                     int (*next)(FmlParser *, ScNode **), int tier)
{
    ScNode *left;
    int rc = next(fp, &left);
    if (rc != SC_OK) return rc;
    for (;;) {
        int op;
        ScNode *right, *node;
        skip_ws(fp);
        op = next_bin_op(fp, tier);
        if (!op) { *out = left; return SC_OK; }
        rc = next(fp, &right);
        if (rc != SC_OK) { free_node(left); return rc; }
        node = make_node(SC_NODE_BINARY);
        if (!node) { free_node(left); free_node(right); return SC_ERR_NOMEM; }
        node->u.binary.op = op;
        node->u.binary.lhs = left;
        node->u.binary.rhs = right;
        left = node;
    }
}

static int parse_multiplication(FmlParser *fp, ScNode **out)
{
    return parse_bin(fp, out, parse_power, 0);
}

static int parse_addition(FmlParser *fp, ScNode **out)
{
    return parse_bin(fp, out, parse_multiplication, 1);
}

static int parse_comparison(FmlParser *fp, ScNode **out)
{
    return parse_bin(fp, out, parse_addition, 2);
}

static int parse_expr(FmlParser *fp, ScNode **out)
{
    return parse_comparison(fp, out);
}

int sci_parse_formula_text(ScWorkbook *wb, int sheet,
                           const char *text, size_t text_len,
                           ScFormula **out)
{
    FmlParser fp;
    ScFormula *fml;
    ScNode *root = NULL;
    char *copy;
    int rc;

    if (!wb || !text || !out) return SC_ERR_INTERNAL;
    if (text_len > SC_MAX_FORMULA_LEN) return SC_ERR_OVERFLOW;
    *out = NULL;

    fp.text = text;
    fp.len = text_len;
    fp.pos = 0;
    fp.wb = wb;
    fp.sheet = sheet;

    skip_ws(&fp);
    if (peek(&fp) == '=') advance(&fp);
    skip_ws(&fp);
    if (!peek(&fp)) return SC_ERR_FORMAT;

    rc = parse_expr(&fp, &root);
    if (rc != SC_OK) { free_node(root); return rc; }
    skip_ws(&fp);
    if (fp.pos != fp.len) { free_node(root); return SC_ERR_FORMAT; }

    copy = (char *)malloc(text_len + 1);
    if (!copy) { free_node(root); return SC_ERR_NOMEM; }
    if (text_len) memcpy(copy, text, text_len);
    copy[text_len] = '\0';

    fml = (ScFormula *)calloc(1, sizeof(*fml));
    if (!fml) { free(copy); free_node(root); return SC_ERR_NOMEM; }
    fml->root = root;
    fml->text = copy;
    fml->text_len = text_len;
    *out = fml;
    return SC_OK;
}
