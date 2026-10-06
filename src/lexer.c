/*
 * DSL lexer.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lexer.h"

#include <stdlib.h>
#include <string.h>

#include "keywords.h"
#include "value.h"

void atree__error_set(atree_error_t *err, atree_status_t status, size_t offset, size_t length,
                      const char *message)
{
    size_t n;
    if (err == NULL) {
        return;
    }
    err->status = status;
    err->offset = offset;
    err->length = length;
    n = strlen(message);
    if (n >= sizeof err->message) {
        n = sizeof err->message - 1;
    }
    memcpy(err->message, message, n);
    err->message[n] = '\0';
}

void atree__error_set_name(atree_error_t *err, atree_status_t status, size_t offset, size_t length,
                           const char *prefix, const char *name, size_t name_len,
                           const char *suffix)
{
    size_t used = 0;
    size_t cap;
    if (err == NULL) {
        return;
    }
    cap = sizeof err->message - 1;
    err->status = status;
    err->offset = offset;
    err->length = length;
#define APPEND(s, n)                                                                               \
    do {                                                                                           \
        size_t take_ = (n);                                                                        \
        if (take_ > cap - used) {                                                                  \
            take_ = cap - used;                                                                    \
        }                                                                                          \
        memcpy(err->message + used, (s), take_);                                                   \
        used += take_;                                                                             \
    } while (0)
    APPEND(prefix, strlen(prefix));
    APPEND("'", 1);
    APPEND(name, name_len);
    APPEND("'", 1);
    APPEND(suffix, strlen(suffix));
#undef APPEND
    err->message[used] = '\0';
}

void atree__lexer_init(struct atree__lexer *lx, const char *text, size_t len)
{
    lx->text = text;
    lx->len = len;
    lx->pos = 0;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static char peek_at(const struct atree__lexer *lx, size_t i)
{
    return i < lx->len ? lx->text[i] : '\0';
}

/* Scans a number starting at lx->pos (optional leading '-' already verified
 * to precede a digit). */
static atree_status_t lex_number(struct atree__lexer *lx, struct atree__token *tok,
                                 atree_error_t *err)
{
    size_t start = lx->pos;
    size_t i = start;
    bool neg = false;
    bool is_float = false;
    uint64_t mag = 0;

    if (peek_at(lx, i) == '-') {
        neg = true;
        i++;
    }
    while (atree__is_digit(peek_at(lx, i))) {
        unsigned d = (unsigned)(peek_at(lx, i) - '0');
        if (!is_float) {
            if (mag > (UINT64_MAX - d) / 10) {
                mag = UINT64_MAX; /* overflow marker; still consume digits */
            } else {
                mag = mag * 10 + d;
            }
        }
        i++;
    }
    if (peek_at(lx, i) == '.') {
        is_float = true;
        i++;
        while (atree__is_digit(peek_at(lx, i))) {
            i++;
        }
    }
    if (peek_at(lx, i) == 'e' || peek_at(lx, i) == 'E') {
        size_t j = i + 1;
        if (peek_at(lx, j) == '+' || peek_at(lx, j) == '-') {
            j++;
        }
        if (atree__is_digit(peek_at(lx, j))) {
            is_float = true;
            while (atree__is_digit(peek_at(lx, j))) {
                j++;
            }
            i = j;
        }
    }
    /* A number must not run straight into an identifier: "12abc" is an error. */
    if (atree__is_ident_start(peek_at(lx, i))) {
        atree__error_set(err, ATREE_ERR_SYNTAX, start, i + 1 - start,
                         "malformed number: letters directly after digits");
        return ATREE_ERR_SYNTAX;
    }
    tok->offset = start;
    tok->length = i - start;
    lx->pos = i;

    if (is_float) {
        char buf[64];
        char *end = NULL;
        double v;
        if (tok->length >= sizeof buf) {
            atree__error_set(err, ATREE_ERR_INVALID_LITERAL, start, tok->length,
                             "float literal too long");
            return ATREE_ERR_INVALID_LITERAL;
        }
        memcpy(buf, lx->text + start, tok->length);
        buf[tok->length] = '\0';
        v = strtod(buf, &end);
        if (end == NULL || *end != '\0' || !atree__double_is_finite(v)) {
            atree__error_set(err, ATREE_ERR_INVALID_LITERAL, start, tok->length,
                             "float literal is not a finite number");
            return ATREE_ERR_INVALID_LITERAL;
        }
        tok->kind = ATREE_TOK_FLOAT;
        tok->f = v;
        return ATREE_OK;
    }

    /* int64 range: magnitude up to 2^63-1, or 2^63 when negative */
    if (mag > (uint64_t)INT64_MAX + (neg ? 1u : 0u)) {
        atree__error_set(err, ATREE_ERR_INVALID_LITERAL, start, tok->length,
                         "integer literal out of range");
        return ATREE_ERR_INVALID_LITERAL;
    }
    tok->kind = ATREE_TOK_INT;
    if (neg) {
        /* -(2^63) cannot be formed by negating an int64, so build it from -(mag-1)-1. */
        tok->i = mag == 0 ? 0 : -(int64_t)(mag - 1) - 1;
    } else {
        tok->i = (int64_t)mag;
    }
    return ATREE_OK;
}

static atree_status_t lex_string(struct atree__lexer *lx, struct atree__token *tok,
                                 atree_error_t *err)
{
    size_t start = lx->pos;
    char quote = lx->text[start];
    size_t i = start + 1;
    for (;;) {
        char c = peek_at(lx, i);
        if (i >= lx->len) {
            atree__error_set(err, ATREE_ERR_SYNTAX, start, lx->len - start,
                             "unterminated string literal");
            return ATREE_ERR_SYNTAX;
        }
        if (c == '\\') {
            if (i + 1 >= lx->len) {
                atree__error_set(err, ATREE_ERR_SYNTAX, start, lx->len - start,
                                 "unterminated escape sequence in string literal");
                return ATREE_ERR_SYNTAX;
            }
            i += 2;
            continue;
        }
        if (c == quote) {
            break;
        }
        i++;
    }
    tok->kind = ATREE_TOK_STRING;
    tok->offset = start;
    tok->length = i + 1 - start;
    lx->pos = i + 1;
    return ATREE_OK;
}

atree_status_t atree__lexer_next(struct atree__lexer *lx, struct atree__token *tok,
                                 atree_error_t *err)
{
    char c;
    char n;

    tok->i = 0;
    tok->f = 0.0;
    while (lx->pos < lx->len && is_space(lx->text[lx->pos])) {
        lx->pos++;
    }
    tok->offset = lx->pos;
    tok->length = 0;
    if (lx->pos >= lx->len) {
        tok->kind = ATREE_TOK_END;
        return ATREE_OK;
    }
    c = lx->text[lx->pos];
    n = peek_at(lx, lx->pos + 1);

#define ONE(k)                                                                                     \
    do {                                                                                           \
        tok->kind = (k);                                                                           \
        tok->length = 1;                                                                           \
        lx->pos += 1;                                                                              \
        return ATREE_OK;                                                                           \
    } while (0)
#define TWO(k)                                                                                     \
    do {                                                                                           \
        tok->kind = (k);                                                                           \
        tok->length = 2;                                                                           \
        lx->pos += 2;                                                                              \
        return ATREE_OK;                                                                           \
    } while (0)

    switch (c) {
    case '(':
        ONE(ATREE_TOK_LPAREN);
    case ')':
        ONE(ATREE_TOK_RPAREN);
    case '[':
        ONE(ATREE_TOK_LBRACKET);
    case ']':
        ONE(ATREE_TOK_RBRACKET);
    case ',':
        ONE(ATREE_TOK_COMMA);
    case '<':
        if (n == '=') {
            TWO(ATREE_TOK_LE);
        }
        if (n == '>') {
            TWO(ATREE_TOK_NE);
        }
        ONE(ATREE_TOK_LT);
    case '>':
        if (n == '=') {
            TWO(ATREE_TOK_GE);
        }
        ONE(ATREE_TOK_GT);
    case '=':
        if (n == '=') {
            TWO(ATREE_TOK_EQ); /* accept == as a convenience */
        }
        ONE(ATREE_TOK_EQ);
    case '!':
        if (n == '=') {
            TWO(ATREE_TOK_NE);
        }
        ONE(ATREE_TOK_BANG);
    case '&':
        if (n == '&') {
            TWO(ATREE_TOK_AND);
        }
        break;
    case '|':
        if (n == '|') {
            TWO(ATREE_TOK_OR);
        }
        break;
    case '"':
    case '\'':
        return lex_string(lx, tok, err);
    case '-':
        if (atree__is_digit(n)) {
            return lex_number(lx, tok, err);
        }
        break;
    default:
        break;
    }
#undef ONE
#undef TWO

    if (atree__is_digit(c)) {
        return lex_number(lx, tok, err);
    }
    if (atree__is_ident_start(c)) {
        size_t i = lx->pos + 1;
        while (atree__is_ident_char(peek_at(lx, i))) {
            i++;
        }
        tok->kind = ATREE_TOK_WORD;
        tok->length = i - lx->pos;
        lx->pos = i;
        return ATREE_OK;
    }
    atree__error_set(err, ATREE_ERR_SYNTAX, lx->pos, 1, "unexpected character");
    return ATREE_ERR_SYNTAX;
}

static char unescape(char c)
{
    switch (c) {
    case 'n':
        return '\n';
    case 't':
        return '\t';
    case 'r':
        return '\r';
    case '0':
        return '\0';
    default:
        return c; /* \" \' \\ and anything else: the character itself */
    }
}

size_t atree__token_string_len(const struct atree__lexer *lx, const struct atree__token *tok)
{
    size_t i = tok->offset + 1;
    size_t end = tok->offset + tok->length - 1;
    size_t n = 0;
    while (i < end) {
        if (lx->text[i] == '\\') {
            i += 2;
        } else {
            i += 1;
        }
        n++;
    }
    return n;
}

void atree__token_string_copy(const struct atree__lexer *lx, const struct atree__token *tok,
                              char *dst)
{
    size_t i = tok->offset + 1;
    size_t end = tok->offset + tok->length - 1;
    while (i < end) {
        if (lx->text[i] == '\\') {
            *dst++ = unescape(lx->text[i + 1]);
            i += 2;
        } else {
            *dst++ = lx->text[i];
            i += 1;
        }
    }
}
