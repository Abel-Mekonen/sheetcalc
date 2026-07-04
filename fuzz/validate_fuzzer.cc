// fuzzer for parse + sc_validate

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

extern "C" {
#include "sheetcalc.h"
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sc_workbook *wb = NULL;
    if (sc_workbook_open(data, size, &wb) == SC_OK) {
        char errbuf[512];
        sc_validate(wb, errbuf, sizeof errbuf);
        sc_workbook_close(wb);
    }
    return 0;
}
