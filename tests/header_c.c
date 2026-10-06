/*
 * Compiled with -std=c99, -std=c11 and -std=c17 (plus -Wpedantic -Werror) to
 * prove the public header is self-contained and strictly conforming.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "atree.h"

int main(void)
{
    atree_config_t cfg;
    atree_config_init(&cfg);
    if (cfg.allocator != NULL || cfg.flags != 0) {
        return 1;
    }
    if (strcmp(atree_version(), ATREE_VERSION_STRING) != 0) {
        return 1;
    }
    if (strcmp(atree_strerror(ATREE_OK), "success") != 0) {
        return 1;
    }
    if (atree_default_allocator() == NULL) {
        return 1;
    }
    printf("atree %s header ok (C)\n", atree_version());
    return 0;
}
