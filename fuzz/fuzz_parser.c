/*
 * libFuzzer / AFL++ harness for the DSL parser, normalization and the
 * reference evaluator. Build with clang -fsanitize=fuzzer (make fuzz), or
 * with -DATREE_FUZZ_MAIN to get a main() that replays files given as
 * arguments (used by `make check` to keep the harness compiling everywhere).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/expr.h"

#include "atree.h"

static atree_t *fuzz_tree;
static atree_event_t *fuzz_event;

static const atree_attr_def_t FUZZ_DEFS[] = {
    {"b", ATREE_TYPE_BOOL},   {"i", ATREE_TYPE_INT},       {"f", ATREE_TYPE_FLOAT},
    {"s", ATREE_TYPE_STRING}, {"il", ATREE_TYPE_INT_LIST}, {"sl", ATREE_TYPE_STRING_LIST},
};

static int fuzz_init(void)
{
    const char *sl[] = {"x", "y"};
    int64_t il[] = {1, 2, 3};
    if (fuzz_tree != NULL) {
        return 0;
    }
    if (atree_create(NULL, FUZZ_DEFS, sizeof FUZZ_DEFS / sizeof FUZZ_DEFS[0], &fuzz_tree) !=
        ATREE_OK) {
        return 1;
    }
    if (atree_event_create(fuzz_tree, &fuzz_event) != ATREE_OK) {
        return 1;
    }
    (void)atree_event_set_bool(fuzz_event, "b", true);
    (void)atree_event_set_int(fuzz_event, "i", 2);
    (void)atree_event_set_float(fuzz_event, "f", 1.5);
    (void)atree_event_set_string(fuzz_event, "s", "x", 1);
    (void)atree_event_set_int_list(fuzz_event, "il", il, 3);
    (void)atree_event_set_string_list(fuzz_event, "sl", sl, NULL, 2);
    return 0;
}

static int sink(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    (void)data;
    (void)len;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    atree_expr_t *e = NULL;
    atree_expr_t *n = NULL;
    atree_error_t err;
    if (fuzz_init() != 0) {
        return 0;
    }
    if (atree_expr_parse(fuzz_tree, (const char *)data, size, &e, &err) == ATREE_OK) {
        (void)atree_expr_print(e, sink, NULL);
        (void)atree_expr_eval(e, fuzz_event);
        if (atree__expr_normalize(e, 64, &n) == ATREE_OK) {
            (void)atree_expr_eval(n, fuzz_event);
            atree_expr_free(n);
        }
        atree_expr_free(e);
    } else {
        /* Diagnostics must point inside the input. */
        if (err.offset != SIZE_MAX && err.offset > size) {
            abort();
        }
    }
    return 0;
}

#ifdef ATREE_FUZZ_MAIN
int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        char *buf;
        long n;
        if (f == NULL) {
            continue;
        }
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = malloc(n > 0 ? (size_t)n : 1);
        if (buf != NULL && n > 0 && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            LLVMFuzzerTestOneInput((const uint8_t *)buf, (size_t)n);
        }
        free(buf);
        fclose(f);
    }
    atree_event_destroy(fuzz_event);
    atree_destroy(fuzz_tree);
    return 0;
}
#endif
