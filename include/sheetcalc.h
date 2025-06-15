/*
 * sheetcalc.h — public API for .scb workbook parse/eval
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

#ifndef SHEETCALC_H
#define SHEETCALC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC_VERSION_MAJOR  0
#define SC_VERSION_MINOR  9
#define SC_VERSION_PATCH  2

/* rc < 0 on failure */
typedef enum {
    SC_OK           = 0,
    SC_ERR_NOMEM    = -1,
    SC_ERR_FORMAT   = -2,
    SC_ERR_OVERFLOW = -3,
    SC_ERR_BADREF   = -4,
    SC_ERR_CYCLE    = -5,
    SC_ERR_TYPE     = -6,
    SC_ERR_RANGE    = -7,
    SC_ERR_IO       = -8,
    SC_ERR_CORRUPT  = -9,
    SC_ERR_LIMIT    = -10,
    SC_ERR_INTERNAL = -99
} sc_err;

typedef enum {
    SC_VAL_EMPTY = 0,
    SC_VAL_NUM,
    SC_VAL_STR,
    SC_VAL_BOOL,
    SC_VAL_ERR,
    SC_VAL_LIST,
    SC_VAL_NIL
} sc_val_type;

typedef struct sc_workbook sc_workbook;
typedef struct sc_sheet    sc_sheet;
typedef struct sc_cell     sc_cell;
typedef struct sc_formula  sc_formula;
typedef struct sc_range    sc_range;

/* --- lifecycle --- */

const char *sc_version(void);

/* data = raw .scb buf; len = byte sz; out receives owned wb */
int sc_workbook_open(const uint8_t *data, size_t len, sc_workbook **out);
void sc_workbook_close(sc_workbook *wb);

/* --- sheets --- */

int sc_workbook_sheet_count(const sc_workbook *wb);
int sc_workbook_get_sheet(sc_workbook *wb, int idx, sc_sheet **out);
int sc_workbook_find_sheet(sc_workbook *wb, const char *name, sc_sheet **out);

const char *sc_sheet_name(const sc_sheet *s);
void sc_sheet_dims(const sc_sheet *s, int *rows, int *cols);  /* 0-based */
int sc_sheet_get_cell(sc_sheet *s, int row, int col, sc_cell **out);

/* --- cell accessors --- */

int sc_cell_type(const sc_cell *c);
int sc_cell_as_num(const sc_cell *c, double *out);
int sc_cell_as_str(const sc_cell *c, const char **out, size_t *len);
int sc_cell_as_bool(const sc_cell *c, int *out);
int sc_cell_has_formula(const sc_cell *c);  /* nonzero => has fml */

/* --- evaluation --- */

int sc_evaluate(sc_workbook *wb);           /* all sheets, dep order */
int sc_evaluate_sheet(sc_workbook *wb, sc_sheet *s);

// errbuf_sz includes NUL; errbuf may be NULL
int sc_validate(const sc_workbook *wb, char *errbuf, size_t errbuf_sz);

/* --- export --- */

/* caller frees *out (malloc'd) */
int sc_export_csv(sc_workbook *wb, int sheet_idx, uint8_t **out, size_t *out_len);
int sc_export_json(sc_workbook *wb, uint8_t **out, size_t *out_len);

/* --- mutation (in-memory wb only) --- */

int sc_convert_normalize(sc_workbook *wb);  /* coerce to canonical vals */

int sc_workbook_add_cells(sc_workbook *wb, int sheet_idx, int count);

// named range: sheet + rect (r0,c0)..(r1,c1)
int sc_workbook_define_name(sc_workbook *wb, const char *name,
                            int sheet, int r0, int c0, int r1, int c1);

#ifdef __cplusplus
}
#endif

#endif /* SHEETCALC_H */
