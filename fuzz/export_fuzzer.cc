// fuzzer for parse + evaluate + JSON export

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
            uint8_t *json = NULL;
            size_t json_len = 0;
            if (sc_export_json(wb, &json, &json_len) == SC_OK)
                free(json);
        }
        sc_workbook_close(wb);
    }
    return 0;
}
