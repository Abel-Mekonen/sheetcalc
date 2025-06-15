# sheetcalc

A lightweight C library for parsing, evaluating, and exporting spreadsheet
workbooks in the CellScript Binary (`.scb`) format. Designed for embedding in
applications that need cell-based computation without pulling in a full
spreadsheet engine.

You pass in a `.scb` buffer; sheetcalc builds an in-memory workbook, evaluates
formulas in dependency order, and exposes results through a small C API or the
bundled CLI.

## Features

- Binary `.scb` parsing with checksum verification
- Formula evaluation with 30+ built-in functions
- Named ranges, multi-sheet support, cross-sheet references
- CSV/JSON export, validation, and in-memory format conversion

## Building

Requires a C11 compiler, `make`, and `ar`. No third-party dependencies.

```sh
git clone https://github.com/Abel-Mekonen/sheetcalc.git
cd sheetcalc
make
make test
make install              # default prefix: /usr/local
```

Use `make install PREFIX=$HOME/.local` for a user-local install. Run
`make asan` when you want an AddressSanitizer build for local debugging.

## Library usage

The public header is `include/sheetcalc.h`. Open a workbook from a raw byte
buffer (read or mmap the file yourself), evaluate, read a cell, then close:

```c
#include "sheetcalc.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    FILE *f = fopen("budget.scb", "rb");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)sz);
    fread(data, 1, (size_t)sz, f);
    fclose(f);

    sc_workbook *wb = NULL;
    sc_workbook_open(data, (size_t)sz, &wb);
    sc_evaluate(wb);

    sc_sheet *sheet;
    sc_workbook_get_sheet(wb, 0, &sheet);
    sc_cell *cell;
    sc_sheet_get_cell(sheet, 0, 0, &cell);

    double total;
    sc_cell_as_num(cell, &total);
    printf("A1 = %.2f\n", total);

    sc_workbook_close(wb);
    free(data);
    return 0;
}
```

Link with `-lsheetcalc -lm`.

## CLI

```sh
sheetcalc info file.scb
sheetcalc eval file.scb
sheetcalc export-csv file.scb 0
```

`info` prints sheet dimensions and named ranges. `eval` validates and evaluates
the workbook, reporting any errors. `export-csv` writes a sheet (by index) to
stdout; `export-json` dumps the full workbook.

## Supported functions

| Category | Functions |
| -------- | --------- |
| Aggregate | `SUM`, `AVG` / `AVERAGE`, `MIN`, `MAX`, `COUNT` |
| Logic | `IF`, `AND`, `OR`, `NOT` |
| Math | `ABS`, `ROUND`, `MOD`, `SQRT`, `LOG`, `LN`, `EXP`, `PI` |
| Text | `CONCAT`, `LEN`, `LEFT`, `RIGHT`, `MID`, `FIND`, `UPPER`, `LOWER`, `TRIM`, `TEXT`, `VALUE` |
| Lookup | `VLOOKUP`, `HLOOKUP`, `INDEX`, `MATCH` |
| Info | `ISBLANK`, `ISERROR`, `ISNA`, `NA` |
| Date/time | `TODAY`, `NOW` |

Arithmetic operators, comparisons, and `&` string concatenation work in
expressions alongside these functions.

## `.scb` format overview

Files begin with a 16-byte header (`CSB\0` magic, version, flags, section
count) and a section table pointing to typed blobs: sheets, cells, formulas,
strings, styles, named ranges, and metadata. Integers are little-endian;
current on-disk version is `1`.

## Roadmap

- Shared library builds (`.so` / `.dylib`)
- Additional date/time functions
- External data source connectors for lookup

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT — see [LICENSE](LICENSE).
