// fuzzer for parse + evaluate + convert/export

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

extern "C" {
#include "sheetcalc.h"
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sc_workbook *wb = NULL;
    if (sc_workbook_open(data, size, &wb) == SC_OK) {
        if (sc_evaluate(wb) == SC_OK) {
            uint8_t *csv = NULL;
            size_t csv_len = 0;
            if (sc_export_csv(wb, 0, &csv, &csv_len) == SC_OK)
                free(csv);
            sc_convert_normalize(wb);
        }
        sc_workbook_close(wb);
    }
    return 0;
}
