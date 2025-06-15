/*
 * test_basic.c — smoke tests for public API + value layer
 */

#include "../include/sheetcalc.h"
#include "../include/sc_internal.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char *name, int ok)
{
    if (ok)
        printf("PASS  %s\n", name);
    else {
        printf("FAIL  %s\n", name);
        failures++;
    }
}

static int test_version(void)
{
    const char *v = sc_version();
    return (v && strcmp(v, "0.9.2") == 0) ? 0 : 1;
}

static int test_open_null(void)
{
    sc_workbook *wb = (sc_workbook *)0x1;
    int rc = sc_workbook_open(NULL, 0, &wb);

    if (wb != NULL)
        return 1;
    return rc != SC_OK ? 0 : 1;
}

static int test_open_short(void)
{
    uint8_t tiny[8] = {0};
    sc_workbook *wb = NULL;
    int rc = sc_workbook_open(tiny, sizeof(tiny), &wb);

    if (wb)
        sc_workbook_close(wb);
    return rc == SC_ERR_FORMAT ? 0 : 1;
}

static int test_open_bad_magic(void)
{
    uint8_t buf[32];
    sc_workbook *wb = NULL;
    int rc;

    memset(buf, 0, sizeof(buf));
    buf[0] = 'X';
    buf[1] = 'X';
    buf[2] = 'X';
    buf[3] = '\0';

    rc = sc_workbook_open(buf, sizeof(buf), &wb);
    if (wb)
        sc_workbook_close(wb);
    return rc == SC_ERR_FORMAT ? 0 : 1;
}

static int test_value_types(void)
{
    ScVal v;

    sc_val_num(&v, 1.5);
    if (v.type != SC_VAL_NUM)
        return 1;
    sc_val_free(&v);

    sc_val_str(&v, "hello");
    if (v.type != SC_VAL_STR)
        return 1;
    sc_val_free(&v);

    sc_val_bool(&v, 1);
    if (v.type != SC_VAL_BOOL)
        return 1;
    sc_val_free(&v);

    sc_val_err(&v, 3);
    if (v.type != SC_VAL_ERR)
        return 1;
    sc_val_free(&v);

    sc_val_nil(&v);
    if (v.type != SC_VAL_NIL)
        return 1;

    {
        ScVal items[2];
        sc_val_num(&items[0], 1.0);
        sc_val_num(&items[1], 2.0);
        sc_val_list(&v, items, 2);
        if (v.type != SC_VAL_LIST || v.as.list.count != 2)
            return 1;
        sc_val_free(&items[0]);
        sc_val_free(&items[1]);
        sc_val_free(&v);
    }

    return 0;
}

static int test_value_copy(void)
{
    ScVal a, b;

    sc_val_str(&a, "copyme");
    if (sc_val_copy(&b, &a) != SC_OK)
        return 1;
    if (b.type != SC_VAL_STR || b.as.str.len != 6)
        return 1;
    if (strcmp(b.as.str.s, "copyme") != 0)
        return 1;
    if (b.as.str.s == a.as.str.s)   /* must be deep copy */
        return 1;

    b.as.str.s[0] = 'X';
    if (a.as.str.s[0] != 'c')
        return 1;

    sc_val_free(&a);
    sc_val_free(&b);
    return 0;
}

typedef int (*test_fn)(void);

static const struct {
    const char *name;
    test_fn fn;
} tests[] = {
    {"version",       test_version},
    {"open_null",     test_open_null},
    {"open_short",    test_open_short},
    {"open_bad_magic", test_open_bad_magic},
    {"value_types",   test_value_types},
    {"value_copy",    test_value_copy},
};

int main(void)
{
    size_t i;

    printf("test_basic (%zu cases)\n", sizeof(tests) / sizeof(tests[0]));
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int rc = tests[i].fn();
        check(tests[i].name, rc == 0);
    }

    if (failures)
        printf("\n%d failure(s)\n", failures);
    else
        printf("\nall passed\n");

    return failures ? 1 : 0;
}
