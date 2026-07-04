// fuzzer for sc_workbook_open / parse

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

extern "C" {
#include "sheetcalc.h"
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sc_workbook *wb = NULL;
    if (sc_workbook_open(data, size, &wb) == SC_OK) {
        sc_workbook_close(wb);
    }
    return 0;
}
