/*
 * DSL lexer. Pull-based, allocation-free: tokens record byte offsets into
 * the input, so diagnostics can point at them, and string tokens record
 * the span between the quotes (the parser unescapes into its own buffer).
 *
 * Tokens
 *   ( ) [ ] ,  < <= > >= = <> !=  && || !
 *   WORD    [A-Za-z_][A-Za-z0-9_-]*   (keywords are WORDs matched by the parser,
 *                                      case-insensitively)
 *   INT     -?[0-9]+                  (must fit int64_t)
 *   FLOAT   -?[0-9]+ ( "." [0-9]* )? ( [eE] [+-]? [0-9]+ )?   with '.' or exponent
 *   STRING  "..." or '...' with \-escapes
 * Whitespace: space, tab, CR, LF.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_LEXER_H
#define ATREE_LEXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atree.h"

enum atree__tok {
    ATREE_TOK_END = 0,
    ATREE_TOK_LPAREN,
    ATREE_TOK_RPAREN,
    ATREE_TOK_LBRACKET,
    ATREE_TOK_RBRACKET,
    ATREE_TOK_COMMA,
    ATREE_TOK_LT,
    ATREE_TOK_LE,
    ATREE_TOK_GT,
    ATREE_TOK_GE,
    ATREE_TOK_EQ,
    ATREE_TOK_NE,
    ATREE_TOK_AND,  /* && */
    ATREE_TOK_OR,   /* || */
    ATREE_TOK_BANG, /* !  */
    ATREE_TOK_WORD,
    ATREE_TOK_INT,
    ATREE_TOK_FLOAT,
    ATREE_TOK_STRING
};

struct atree__token {
    enum atree__tok kind;
    size_t offset; /* byte offset of the token's first character */
    size_t length; /* token length in bytes (strings: including quotes) */
    int64_t i;     /* ATREE_TOK_INT */
    double f;      /* ATREE_TOK_FLOAT */
};

struct atree__lexer {
    const char *text;
    size_t len;
    size_t pos;
};

void atree__lexer_init(struct atree__lexer *lx, const char *text, size_t len);

/* Reads the next token. On a lexical error returns ATREE_ERR_SYNTAX or
 * ATREE_ERR_INVALID_LITERAL and fills *err (if non-NULL). */
atree_status_t atree__lexer_next(struct atree__lexer *lx, struct atree__token *tok,
                                 atree_error_t *err);

/* The text of a WORD token. */
static inline const char *atree__token_text(const struct atree__lexer *lx,
                                            const struct atree__token *tok)
{
    return lx->text + tok->offset;
}

/* Unescaped length of a STRING token's contents (never more than length - 2). */
size_t atree__token_string_len(const struct atree__lexer *lx, const struct atree__token *tok);
/* Writes the unescaped contents into dst (at least atree__token_string_len bytes). */
void atree__token_string_copy(const struct atree__lexer *lx, const struct atree__token *tok,
                              char *dst);

/* Fills an atree_error_t; message is truncated to the buffer. */
void atree__error_set(atree_error_t *err, atree_status_t status, size_t offset, size_t length,
                      const char *message);
/* Like atree__error_set with a message of the form "<prefix>'<name>'<suffix>". */
void atree__error_set_name(atree_error_t *err, atree_status_t status, size_t offset, size_t length,
                           const char *prefix, const char *name, size_t name_len,
                           const char *suffix);

#endif /* ATREE_LEXER_H */
