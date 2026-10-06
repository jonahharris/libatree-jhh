/*
 * Compiled as C++ to prove the public header works behind extern "C".
 *
 * SPDX-License-Identifier: MIT
 */
#include <cstdio>
#include <cstring>

#include "atree.h"

int main()
{
    atree_config_t cfg;
    atree_config_init(&cfg);
    if (cfg.lock != NULL) {
        return 1;
    }
    if (std::strcmp(atree_version(), ATREE_VERSION_STRING) != 0) {
        return 1;
    }
    const atree_status_t st = ATREE_ERR_NOMEM;
    if (std::strcmp(atree_strerror(st), "out of memory") != 0) {
        return 1;
    }
    std::printf("atree %s header ok (C++)\n", atree_version());
    return 0;
}
