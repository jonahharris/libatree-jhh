/*
 * DSL keywords and identifier syntax.
 *
 * SPDX-License-Identifier: MIT
 */
#include "keywords.h"

#include <string.h>

static const char *const atree__keywords[] = {
    "and",  "or",  "not", "xor",  "xnor",  "in",   "of",    "one",
    "none", "all", "is",  "null", "empty", "true", "false", "between",
};

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool atree__ascii_ieq(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t i;
    if (alen != blen) {
        return false;
    }
    for (i = 0; i < alen; i++) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

bool atree__is_identifier(const char *s, size_t len)
{
    size_t i;
    if (len == 0 || !atree__is_ident_start(s[0])) {
        return false;
    }
    for (i = 1; i < len; i++) {
        if (!atree__is_ident_char(s[i])) {
            return false;
        }
    }
    return true;
}

bool atree__is_keyword(const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < sizeof atree__keywords / sizeof atree__keywords[0]; i++) {
        if (atree__ascii_ieq(s, len, atree__keywords[i], strlen(atree__keywords[i]))) {
            return true;
        }
    }
    return false;
}
