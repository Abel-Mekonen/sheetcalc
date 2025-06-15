/*
 * gen_seeds.c — generate .scb seed corpora for fuzz harnesses
 */

#include "../include/sc_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

enum {
    SC_SEC_SHEETS = 1, SC_SEC_CELLS, SC_SEC_FORMULAS,
    SC_SEC_STYLES = 5, SC_SEC_NAMES
};

typedef struct { uint8_t *data; size_t len, cap; } Wbuf;
typedef struct { uint32_t type; Wbuf body; } Sec;

typedef struct {
    uint16_t sheet;
    uint32_t row, col;
    uint8_t val_type;
    double num;
    const char *str;
    uint8_t boolean;
    int32_t err_code;
    int32_t formula_idx;
    uint32_t style_id;
} CellSpec;

static int wgrow(Wbuf *b, size_t need)
{
    size_t nc = b->cap ? b->cap * 2 : 256;
    uint8_t *p;
    if (b->len + need <= b->cap)
        return 0;
    while (nc < b->len + need)
        nc *= 2;
    p = (uint8_t *)realloc(b->data, nc);
    if (!p)
        return -1;
    b->data = p;
    b->cap = nc;
    return 0;
}

static int wput(Wbuf *b, const void *src, size_t n)
{
    if (n == 0)
        return 0;
    if (wgrow(b, n) != 0)
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int wu8(Wbuf *b, uint8_t v) { return wput(b, &v, 1); }

static int wu16(Wbuf *b, uint16_t v)
{
    uint8_t le[2] = { (uint8_t)(v & 0xffu), (uint8_t)(v >> 8) };
    return wput(b, le, 2);
}

static int wu32(Wbuf *b, uint32_t v)
{
    uint8_t le[4] = {
        (uint8_t)(v & 0xffu), (uint8_t)((v >> 8) & 0xffu),
        (uint8_t)((v >> 16) & 0xffu), (uint8_t)((v >> 24) & 0xffu)
    };
    return wput(b, le, 4);
}

static int wi32(Wbuf *b, int32_t v) { return wu32(b, (uint32_t)v); }

static int wf64(Wbuf *b, double v)
{
    union { uint64_t u; double d; } x;
    x.d = v;
    return wu32(b, (uint32_t)(x.u & 0xffffffffu)) ||
           wu32(b, (uint32_t)(x.u >> 32));
}

static int wstr16(Wbuf *b, const char *s)
{
    size_t n = strlen(s);
    return wu16(b, (uint16_t)n) || wput(b, s, n);
}

static int wcell_val(Wbuf *b, const CellSpec *c)
{
    if (wu8(b, c->val_type) != 0)
        return -1;
    switch (c->val_type) {
    case SC_VAL_NUM:  return wf64(b, c->num);
    case SC_VAL_STR:
        return wu32(b, (uint32_t)strlen(c->str)) ||
               wput(b, c->str, strlen(c->str));
    case SC_VAL_BOOL: return wu8(b, c->boolean ? 1 : 0);
    case SC_VAL_ERR:  return wi32(b, c->err_code);
    default:          return 0;
    }
}

static int wsheets(Wbuf *b, const char *name, uint32_t nr, uint32_t nc)
{
    return wu16(b, 1) || wstr16(b, name) || wu32(b, nr) ||
           wu32(b, nc) || wu32(b, 0);
}

static int wcells(Wbuf *b, const CellSpec *cells, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        const CellSpec *c = &cells[i];
        if (wu16(b, c->sheet) || wu32(b, c->row) || wu32(b, c->col) ||
            wcell_val(b, c) || wi32(b, c->formula_idx) || wu32(b, c->style_id))
            return -1;
    }
    return 0;
}

static int wformulas(Wbuf *b, const char **texts, int n)
{
    int i;
    if (wu32(b, (uint32_t)n) != 0)
        return -1;
    for (i = 0; i < n; i++) {
        size_t tlen = strlen(texts[i]);
        if (wu32(b, (uint32_t)tlen) || wput(b, texts[i], tlen))
            return -1;
    }
    return 0;
}

static int wstyle_one(Wbuf *b, uint32_t fmt, uint32_t font, uint32_t fill,
                      uint32_t border, uint8_t align, uint8_t wrap)
{
    return wu32(b, fmt) || wu32(b, font) || wu32(b, fill) ||
           wu32(b, border) || wu8(b, align) || wu8(b, wrap);
}

static int wnames(Wbuf *b, const char *name, int32_t sh,
                  uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1)
{
    return wu16(b, 1) || wstr16(b, name) || wi32(b, sh) ||
           wu32(b, r0) || wu32(b, c0) || wu32(b, r1) || wu32(b, c1);
}

static int csb_write(const char *path, Sec *secs, int nsecs)
{
    FILE *fp = fopen(path, "wb");
    uint8_t hdr[12];
    uint32_t off;
    int i;

    if (!fp)
        return -1;

    hdr[0] = 1; hdr[1] = hdr[2] = hdr[3] = 0;
    hdr[4] = hdr[5] = hdr[6] = hdr[7] = 0;
    hdr[8]  = (uint8_t)(nsecs);
    hdr[9]  = (uint8_t)(nsecs >> 8);
    hdr[10] = (uint8_t)(nsecs >> 16);
    hdr[11] = (uint8_t)(nsecs >> 24);

    if (fwrite("CSB\0", 1, 4, fp) != 4 || fwrite(hdr, 1, 12, fp) != 12)
        goto fail;

    off = 16u + (uint32_t)nsecs * 12u;
    for (i = 0; i < nsecs; i++) {
        uint8_t ent[12];
        uint32_t ty = secs[i].type;
        uint32_t sz = (uint32_t)secs[i].body.len;
        ent[0]  = (uint8_t)(ty);       ent[1]  = (uint8_t)(ty >> 8);
        ent[2]  = (uint8_t)(ty >> 16); ent[3]  = (uint8_t)(ty >> 24);
        ent[4]  = (uint8_t)(off);      ent[5]  = (uint8_t)(off >> 8);
        ent[6]  = (uint8_t)(off >> 16);ent[7]  = (uint8_t)(off >> 24);
        ent[8]  = (uint8_t)(sz);       ent[9]  = (uint8_t)(sz >> 8);
        ent[10] = (uint8_t)(sz >> 16); ent[11] = (uint8_t)(sz >> 24);
        if (fwrite(ent, 1, 12, fp) != 12)
            goto fail;
        off += sz;
    }
    for (i = 0; i < nsecs; i++)
        if (secs[i].body.len &&
            fwrite(secs[i].body.data, 1, secs[i].body.len, fp) != secs[i].body.len)
            goto fail;

    return fclose(fp) == 0 ? 0 : -1;

fail:
    fclose(fp);
    return -1;
}

#define EMPT -1

static CellSpec mkcell(uint16_t sheet, uint32_t row, uint32_t col,
                       uint8_t val_type, double num, const char *str,
                       uint8_t boolean, int32_t err_code,
                       int32_t formula_idx, uint32_t style_id)
{
    CellSpec c;
    c.sheet = sheet; c.row = row; c.col = col; c.val_type = val_type;
    c.num = num; c.str = str; c.boolean = boolean; c.err_code = err_code;
    c.formula_idx = formula_idx; c.style_id = style_id;
    return c;
}

static int emit(const char *dir, const char *fname, Sec *secs, int n)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, fname);
    return csb_write(path, secs, n);
}

static int seed_minimal(const char *dir)
{
    CellSpec cells[] = { mkcell(0,0,0,SC_VAL_EMPTY,0,NULL,0,0,EMPT,0) };
    Sec s[2];
    memset(s, 0, sizeof(s));
    s[0].type = SC_SEC_SHEETS;
    s[1].type = SC_SEC_CELLS;
    return wsheets(&s[0].body, "Sheet1", 1, 1) ||
           wcells(&s[1].body, cells, 1) ||
           emit(dir, "seed_minimal.scb", s, 2);
}

static int seed_numbers(const char *dir)
{
    CellSpec cells[9];
    int r, c, i = 0;
    Sec s[2];
    memset(s, 0, sizeof(s));
    s[0].type = SC_SEC_SHEETS;
    s[1].type = SC_SEC_CELLS;
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            cells[i++] = mkcell(0,(uint32_t)r,(uint32_t)c,SC_VAL_NUM,
                                (double)(r*3+c+1),NULL,0,0,EMPT,0);
    return wsheets(&s[0].body, "Data", 3, 3) ||
           wcells(&s[1].body, cells, 9) ||
           emit(dir, "seed_numbers.scb", s, 2);
}

static int seed_strings(const char *dir)
{
    static const char *names[] = {
        "Alice","Bob","Carol","Dave","Eve",
        "Frank","Grace","Heidi","Ivan","Judy"
    };
    CellSpec cells[10];
    int i;
    Sec s[2];
    memset(s, 0, sizeof(s));
    s[0].type = SC_SEC_SHEETS;
    s[1].type = SC_SEC_CELLS;
    for (i = 0; i < 10; i++)
        cells[i] = mkcell(0,(uint32_t)(i/2),(uint32_t)(i%2),SC_VAL_STR,
                         0,names[i],0,0,EMPT,0);
    return wsheets(&s[0].body, "Names", 5, 2) ||
           wcells(&s[1].body, cells, 10) ||
           emit(dir, "seed_strings.scb", s, 2);
}

static int seed_formulas(const char *dir)
{
    const char *fmls[] = { "=A1+B1", "=SUM(A1:A5)" };
    CellSpec cells[] = {
        mkcell(0,0,0,SC_VAL_NUM,1,NULL,0,0,EMPT,0),
        mkcell(0,0,1,SC_VAL_NUM,2,NULL,0,0,EMPT,0),
        mkcell(0,0,2,SC_VAL_EMPTY,0,NULL,0,0,0,0),
        mkcell(0,1,0,SC_VAL_NUM,10,NULL,0,0,EMPT,0),
        mkcell(0,1,1,SC_VAL_NUM,20,NULL,0,0,EMPT,0),
        mkcell(0,1,2,SC_VAL_EMPTY,0,NULL,0,0,1,0),
        mkcell(0,2,0,SC_VAL_NUM,30,NULL,0,0,EMPT,0),
        mkcell(0,3,0,SC_VAL_NUM,40,NULL,0,0,EMPT,0),
        mkcell(0,4,0,SC_VAL_NUM,50,NULL,0,0,EMPT,0),
    };
    Sec s[3];
    memset(s, 0, sizeof(s));
    s[0].type = SC_SEC_SHEETS;
    s[1].type = SC_SEC_FORMULAS;
    s[2].type = SC_SEC_CELLS;
    return wsheets(&s[0].body, "Calc", 5, 3) ||
           wformulas(&s[1].body, fmls, 2) ||
           wcells(&s[2].body, cells, 9) ||
           emit(dir, "seed_formulas.scb", s, 3);
}

static int wsheets_n(Wbuf *b, const char **names, uint32_t *dims, int n)
{
    int i;
    if (wu16(b, (uint16_t)n) != 0)
        return -1;
    for (i = 0; i < n; i++)
        if (wstr16(b, names[i]) || wu32(b, dims[i*2]) ||
            wu32(b, dims[i*2+1]) || wu32(b, 0))
            return -1;
    return 0;
}

static int seed_multisheet(const char *dir)
{
    const char *sn[] = { "Values", "Summary" };
    uint32_t dim[] = { 3, 2, 2, 2 };
    const char *fmls[] = { "=Values!A1+Values!B1", "=Values!A2*Values!B2" };
    CellSpec cells[] = {
        mkcell(0,0,0,SC_VAL_NUM,10,NULL,0,0,EMPT,0),
        mkcell(0,0,1,SC_VAL_NUM,5,NULL,0,0,EMPT,0),
        mkcell(0,1,0,SC_VAL_NUM,3,NULL,0,0,EMPT,0),
        mkcell(0,1,1,SC_VAL_NUM,7,NULL,0,0,EMPT,0),
        mkcell(0,2,0,SC_VAL_STR,0,"total",0,0,EMPT,0),
        mkcell(1,0,0,SC_VAL_EMPTY,0,NULL,0,0,0,0),
        mkcell(1,0,1,SC_VAL_EMPTY,0,NULL,0,0,1,0),
        mkcell(1,1,0,SC_VAL_NUM,100,NULL,0,0,EMPT,0),
    };
    Sec s[3];
    memset(s, 0, sizeof(s));
    s[0].type = SC_SEC_SHEETS;
    s[1].type = SC_SEC_FORMULAS;
    s[2].type = SC_SEC_CELLS;
    return wsheets_n(&s[0].body, sn, dim, 2) ||
           wformulas(&s[1].body, fmls, 2) ||
           wcells(&s[2].body, cells, 8) ||
           emit(dir, "seed_multisheet.scb", s, 3);
}

static int seed_complex(const char *dir)
{
    const char *fmls[] = {
        "=IF(A1>0,\"yes\",\"no\")", "=SUM(B1:B10)",
        "=A5/B5", "=AND(C1,C2)"
    };
    CellSpec cells[] = {
        mkcell(0,0,0,SC_VAL_NUM,42,NULL,0,0,EMPT,1),
        mkcell(0,0,1,SC_VAL_STR,0,"label",0,0,EMPT,0),
        mkcell(0,0,2,SC_VAL_BOOL,0,NULL,1,0,EMPT,2),
        mkcell(0,0,3,SC_VAL_ERR,0,NULL,0,4,EMPT,0),
        mkcell(0,0,4,SC_VAL_EMPTY,0,NULL,0,0,EMPT,0),
        mkcell(0,1,0,SC_VAL_NUM,1,NULL,0,0,EMPT,0),
        mkcell(0,1,1,SC_VAL_NUM,2,NULL,0,0,EMPT,0),
        mkcell(0,1,2,SC_VAL_BOOL,0,NULL,0,0,EMPT,0),
        mkcell(0,2,0,SC_VAL_NUM,10,NULL,0,0,EMPT,0),
        mkcell(0,2,1,SC_VAL_NUM,20,NULL,0,0,EMPT,0),
        mkcell(0,3,0,SC_VAL_NUM,5,NULL,0,0,EMPT,0),
        mkcell(0,4,0,SC_VAL_NUM,0,NULL,0,0,EMPT,0),
        mkcell(0,4,1,SC_VAL_NUM,8,NULL,0,0,EMPT,0),
        mkcell(0,5,0,SC_VAL_EMPTY,0,NULL,0,0,0,0),
        mkcell(0,6,0,SC_VAL_EMPTY,0,NULL,0,0,1,0),
        mkcell(0,7,0,SC_VAL_EMPTY,0,NULL,0,0,2,0),
        mkcell(0,8,0,SC_VAL_EMPTY,0,NULL,0,0,3,0),
        mkcell(0,9,2,SC_VAL_STR,0,"done",0,0,EMPT,1),
    };
    Sec s[5];
    memset(s, 0, sizeof(s));
    s[0].type = SC_SEC_SHEETS;
    s[1].type = SC_SEC_FORMULAS;
    s[2].type = SC_SEC_STYLES;
    s[3].type = SC_SEC_NAMES;
    s[4].type = SC_SEC_CELLS;
    if (wsheets(&s[0].body, "Mixed", 10, 5) ||
        wformulas(&s[1].body, fmls, 4))
        return -1;
    if (wu32(&s[2].body, 3) ||
        wstyle_one(&s[2].body, 1,0,0xFFCC00,0x0F,1,0) ||
        wstyle_one(&s[2].body, 2,1,0x00AAFF,0x03,2,1) ||
        wstyle_one(&s[2].body, 3,2,0xFFFFFF,0x00,0,0) ||
        wnames(&s[3].body, "DataBlock", 0, 0,0, 9,4) ||
        wcells(&s[4].body, cells, 18))
        return -1;
    return emit(dir, "seed_complex.scb", s, 5);
}

static int ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir;
    int rc = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <output_dir>\n", argv[0]);
        return 1;
    }
    dir = argv[1];
    if (ensure_dir(dir) != 0) {
        perror(dir);
        return 1;
    }

    if (seed_minimal(dir))    { rc = 1; fprintf(stderr, "seed_minimal failed\n"); }
    if (seed_numbers(dir))    { rc = 1; fprintf(stderr, "seed_numbers failed\n"); }
    if (seed_strings(dir))    { rc = 1; fprintf(stderr, "seed_strings failed\n"); }
    if (seed_formulas(dir))   { rc = 1; fprintf(stderr, "seed_formulas failed\n"); }
    if (seed_multisheet(dir)) { rc = 1; fprintf(stderr, "seed_multisheet failed\n"); }
    if (seed_complex(dir))    { rc = 1; fprintf(stderr, "seed_complex failed\n"); }

    if (rc == 0)
        printf("wrote 6 seed files to %s\n", dir);
    return rc;
}
