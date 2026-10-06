/*
 * DSL keywords and identifier syntax, shared by the attribute table (names
 * must be identifiers and must not be keywords) and the lexer.
 *
 * Keywords are matched case-insensitively; identifiers are case-sensitive.
 * No <ctype.h>: its functions depend on the locale and take int.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_KEYWORDS_H
#define ATREE_KEYWORDS_H

#include <stdbool.h>
#include <stddef.h>

/* [A-Za-z_][A-Za-z0-9_-]* */
bool atree__is_identifier(const char *s, size_t len);

/* and or not xor xnor in of one none all is null empty true false between */
bool atree__is_keyword(const char *s, size_t len);

/* ASCII case-insensitive equality of two byte ranges. */
bool atree__ascii_ieq(const char *a, size_t alen, const char *b, size_t blen);

static inline bool atree__is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static inline bool atree__is_ident_char(char c)
{
    return atree__is_ident_start(c) || (c >= '0' && c <= '9') || c == '-';
}

static inline bool atree__is_digit(char c)
{
    return c >= '0' && c <= '9';
}

#endif /* ATREE_KEYWORDS_H */
