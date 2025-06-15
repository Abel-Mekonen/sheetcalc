#include "../include/sheetcalc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *prog;

static void usage(void)
{
    fprintf(stderr,
        "usage: %s info|eval|export-csv|export-json|validate <file.scb> [sheet_idx]\n"
        "       %s version\n", prog, prog);
}

static const char *err_name(int rc)
{
    switch (rc) {
    case SC_OK:           return "ok";
    case SC_ERR_NOMEM:    return "out of memory";
    case SC_ERR_FORMAT:   return "bad format";
    case SC_ERR_OVERFLOW: return "overflow";
    case SC_ERR_BADREF:   return "bad reference";
    case SC_ERR_CYCLE:    return "cycle";
    case SC_ERR_TYPE:     return "type error";
    case SC_ERR_RANGE:    return "range error";
    case SC_ERR_IO:       return "io error";
    case SC_ERR_CORRUPT:  return "corrupt";
    case SC_ERR_LIMIT:    return "limit exceeded";
    case SC_ERR_INTERNAL: return "internal error";
    default:              return "unknown error";
    }
}

static int read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *fp;
    long sz;
    uint8_t *buf;

    fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        perror(path); fclose(fp); return -1;
    }
    buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) {
        fprintf(stderr, "malloc failed\n");
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fprintf(stderr, "%s: short read\n", path);
        free(buf); fclose(fp); return -1;
    }
    fclose(fp);
    buf[sz] = '\0';
    *data = buf;
    *len = (size_t)sz;
    return 0;
}

static int open_wb(const char *path, sc_workbook **wb)
{
    uint8_t *data = NULL;
    size_t len = 0;
    int rc;

    if (read_file(path, &data, &len) != 0)
        return SC_ERR_IO;
    rc = sc_workbook_open(data, len, wb);
    free(data);
    if (rc != SC_OK)
        fprintf(stderr, "open %s: %s (%d)\n", path, err_name(rc), rc);
    return rc;
}

static void col_label(int col, char *buf, size_t n)
{
    int k = col, i = 0;
    char t[12];
    while (k >= 0 && i < (int)sizeof(t))
        t[i++] = (char)('A' + (k % 26)), k = k / 26 - 1;
    buf[0] = '\0';
    while (i > 0 && n > 1)
        *buf++ = t[--i], n--;
    *buf = '\0';
}

static int cell_used(const sc_cell *c)
{
    int t;
    if (!c || sc_cell_has_formula(c))
        return c != NULL;
    t = sc_cell_type(c);
    return t != SC_VAL_EMPTY && t != SC_VAL_NIL;
}

static void print_val(const sc_cell *cl)
{
    double n;
    const char *s;
    size_t sl;
    int b;

    switch (sc_cell_type(cl)) {
    case SC_VAL_NUM:
        if (sc_cell_as_num(cl, &n) == SC_OK) printf("%.10g", n);
        break;
    case SC_VAL_STR:
        if (sc_cell_as_str(cl, &s, &sl) == SC_OK) fwrite(s, 1, sl, stdout);
        break;
    case SC_VAL_BOOL:
        if (sc_cell_as_bool(cl, &b) == SC_OK) printf("%s", b ? "TRUE" : "FALSE");
        break;
    case SC_VAL_ERR:
        printf("#ERR");
        break;
    default:
        break;
    }
}

static int cmd_info(const char *path)
{
    sc_workbook *wb = NULL;
    int i, nsh;

    if (open_wb(path, &wb) != SC_OK)
        return 1;
    nsh = sc_workbook_sheet_count(wb);
    printf("sheetcalc workbook: %s\nversion: %s\nsheets: %d\n",
           path, sc_version(), nsh);

    for (i = 0; i < nsh; i++) {
        sc_sheet *sh;
        int rows, cols, r, c, cells = 0;
        sc_cell *cl;

        if (sc_workbook_get_sheet(wb, i, &sh) != SC_OK)
            continue;
        sc_sheet_dims(sh, &rows, &cols);
        for (r = 0; r < rows; r++)
            for (c = 0; c < cols; c++)
                if (sc_sheet_get_cell(sh, r, c, &cl) == SC_OK && cell_used(cl))
                    cells++;
        printf("  [%d] %s  rows=%d cols=%d cells=%d\n",
               i, sc_sheet_name(sh) ? sc_sheet_name(sh) : "(unnamed)",
               rows, cols, cells);
    }
    sc_workbook_close(wb);
    return 0;
}

static int cmd_eval(const char *path)
{
    sc_workbook *wb = NULL;
    int si, nsh, rc;

    if (open_wb(path, &wb) != SC_OK)
        return 1;
    rc = sc_evaluate(wb);
    if (rc != SC_OK) {
        fprintf(stderr, "evaluate: %s (%d)\n", err_name(rc), rc);
        sc_workbook_close(wb); return 1;
    }
    nsh = sc_workbook_sheet_count(wb);
    for (si = 0; si < nsh; si++) {
        sc_sheet *sh;
        int rows, cols, r, c;
        if (sc_workbook_get_sheet(wb, si, &sh) != SC_OK)
            continue;
        sc_sheet_dims(sh, &rows, &cols);
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                sc_cell *cl;
                char addr[12];
                if (sc_sheet_get_cell(sh, r, c, &cl) != SC_OK || !cell_used(cl))
                    continue;
                col_label(c, addr, sizeof(addr));
                printf("%s!%s%d = ", sc_sheet_name(sh), addr, r + 1);
                print_val(cl);
                putchar('\n');
            }
        }
    }
    sc_workbook_close(wb);
    return 0;
}

static int cmd_export(const char *path, int csv, int sheet_idx)
{
    sc_workbook *wb = NULL;
    uint8_t *out = NULL;
    size_t out_len = 0;
    int rc;

    if (open_wb(path, &wb) != SC_OK)
        return 1;
    rc = csv ? sc_export_csv(wb, sheet_idx, &out, &out_len)
             : sc_export_json(wb, &out, &out_len);
    if (rc != SC_OK) {
        fprintf(stderr, "export: %s (%d)\n", err_name(rc), rc);
        sc_workbook_close(wb);
        return 1;
    }
    if (out && out_len)
        fwrite(out, 1, out_len, stdout);
    if (!csv)
        putchar('\n');
    free(out);
    sc_workbook_close(wb);
    return 0;
}

static int cmd_validate(const char *path)
{
    sc_workbook *wb = NULL;
    char err[512];
    int rc;

    if (open_wb(path, &wb) != SC_OK)
        return 1;
    err[0] = '\0';
    rc = sc_validate(wb, err, sizeof(err));
    if (rc != SC_OK) {
        fprintf(stderr, "validate failed: %s (%d)\n", err_name(rc), rc);
        if (err[0])
            fprintf(stderr, "  %s\n", err);
        sc_workbook_close(wb);
        return 1;
    }
    printf("ok: %s\n", path);
    sc_workbook_close(wb);
    return 0;
}

int main(int argc, char **argv)
{
    prog = argc > 0 ? argv[0] : "sheetcalc";
    if (argc < 2) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "version") == 0) {
        printf("sheetcalc %s\n", sc_version());
        return 0;
    }
    if (argc < 3) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "info") == 0)
        return cmd_info(argv[2]);
    if (strcmp(argv[1], "eval") == 0)
        return cmd_eval(argv[2]);
    if (strcmp(argv[1], "export-json") == 0)
        return cmd_export(argv[2], 0, 0);
    if (strcmp(argv[1], "validate") == 0)
        return cmd_validate(argv[2]);
    if (strcmp(argv[1], "export-csv") == 0) {
        char *end;
        long idx;
        if (argc < 4) {
            usage();
            return 1;
        }
        idx = strtol(argv[3], &end, 10);
        if (*end || idx < 0) {
            fprintf(stderr, "bad sheet index: %s\n", argv[3]);
            return 1;
        }
        return cmd_export(argv[2], 1, (int)idx);
    }
    usage();
    return 1;
}
