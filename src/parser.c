/*
 * DSL parser: recursive descent over the lexer, producing an expression
 * through the constructors in expr.h. Never writes to the tree.
 *
 *   expr      := or_expr
 *   or_expr   := xor_expr  ( ("or" | "||") xor_expr )*
 *   xor_expr  := and_expr  ( ("xor" | "xnor") and_expr )*          left-assoc
 *   and_expr  := not_expr  ( ("and" | "&&") not_expr )*
 *   not_expr  := ("not" | "!") not_expr | primary
 *   primary   := "(" expr ")" | "true" | "false"
 *              | ident [ cmp number | eq literal | ("in"|"not in") list
 *                      | ("one"|"none"|"all") "of" list
 *                      | "is" ["not"] ("null"|"empty")
 *                      | "between" number "and" number ]
 *              | number cmp ident | literal eq ident | literal "in" ident
 *   list      := "[" literal ("," literal)* "]" | "(" literal ("," literal)* ")"
 *
 * Precedence, tightest first: not, and, xor/xnor, or (as in C: ! && ^ ||).
 * Keywords are case-insensitive. Nesting depth (parentheses and not) is
 * bounded by the tree's max_depth.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "atree_internal.h"
#include "expr.h"
#include "keywords.h"
#include "lexer.h"
#include "vec.h"

ATREE_VEC_DEFINE(pexprvec, atree_expr_t *);
ATREE_VEC_DEFINE(pi64vec, int64_t);
ATREE_VEC_DEFINE(pstrvec, char *);
ATREE_VEC_DEFINE(psizevec, size_t);

struct parser {
    const atree_t *tree;
    struct atree__lexer lx;
    struct atree__token cur; /* one-token lookahead */
    atree_error_t *err;
    struct atree__mem tmp; /* scratch (list buffers); balanced per call */
    uint32_t depth;
    atree_status_t st; /* first error; parsing stops once set */
};

/* ---- token helpers ------------------------------------------------------ */

static void fail(struct parser *p, atree_status_t st, size_t offset, size_t length,
                 const char *message)
{
    if (p->st == ATREE_OK) {
        p->st = st;
        atree__error_set(p->err, st, offset, length, message);
    }
}

static void fail_name(struct parser *p, atree_status_t st, size_t offset, size_t length,
                      const char *prefix, const char *name, size_t name_len, const char *suffix)
{
    if (p->st == ATREE_OK) {
        p->st = st;
        atree__error_set_name(p->err, st, offset, length, prefix, name, name_len, suffix);
    }
}

static void advance(struct parser *p)
{
    atree_status_t st;
    if (p->st != ATREE_OK) {
        return;
    }
    /* The lexer writes the caller's diagnostic directly (it accepts NULL);
     * copying a local would carry the unwritten tail of its message. */
    st = atree__lexer_next(&p->lx, &p->cur, p->err);
    if (st != ATREE_OK) {
        p->st = st;
        p->cur.kind = ATREE_TOK_END;
    }
}

static bool is_word(const struct parser *p, const struct atree__token *tok, const char *kw)
{
    return tok->kind == ATREE_TOK_WORD &&
        atree__ascii_ieq(atree__token_text(&p->lx, tok), tok->length, kw, strlen(kw));
}

static bool cur_is_word(const struct parser *p, const char *kw)
{
    return is_word(p, &p->cur, kw);
}

/* Looks at the token after the current one without consuming anything. */
static bool next_is_word(const struct parser *p, const char *kw)
{
    struct atree__lexer save = p->lx;
    struct atree__token tok;
    atree_error_t ignored;
    if (atree__lexer_next(&save, &tok, &ignored) != ATREE_OK) {
        return false;
    }
    return is_word(p, &tok, kw);
}

static bool expect(struct parser *p, enum atree__tok kind, const char *what)
{
    if (p->st != ATREE_OK) {
        return false;
    }
    if (p->cur.kind != kind) {
        fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length, what);
        return false;
    }
    advance(p);
    return true;
}

static bool is_literal(const struct atree__token *tok)
{
    return tok->kind == ATREE_TOK_INT || tok->kind == ATREE_TOK_FLOAT ||
        tok->kind == ATREE_TOK_STRING;
}

static bool cmp_token(enum atree__tok kind, atree_op_t *op)
{
    switch (kind) {
    case ATREE_TOK_LT:
        *op = ATREE_OP_LT;
        return true;
    case ATREE_TOK_LE:
        *op = ATREE_OP_LE;
        return true;
    case ATREE_TOK_GT:
        *op = ATREE_OP_GT;
        return true;
    case ATREE_TOK_GE:
        *op = ATREE_OP_GE;
        return true;
    case ATREE_TOK_EQ:
        *op = ATREE_OP_EQ;
        return true;
    case ATREE_TOK_NE:
        *op = ATREE_OP_NE;
        return true;
    case ATREE_TOK_END:
    case ATREE_TOK_LPAREN:
    case ATREE_TOK_RPAREN:
    case ATREE_TOK_LBRACKET:
    case ATREE_TOK_RBRACKET:
    case ATREE_TOK_COMMA:
    case ATREE_TOK_AND:
    case ATREE_TOK_OR:
    case ATREE_TOK_BANG:
    case ATREE_TOK_WORD:
    case ATREE_TOK_INT:
    case ATREE_TOK_FLOAT:
    case ATREE_TOK_STRING:
    default:
        return false;
    }
}

/* `5 < x` means `x > 5`: mirror the operator. */
static atree_op_t mirror(atree_op_t op)
{
    switch (op) {
    case ATREE_OP_LT:
        return ATREE_OP_GT;
    case ATREE_OP_LE:
        return ATREE_OP_GE;
    case ATREE_OP_GT:
        return ATREE_OP_LT;
    case ATREE_OP_GE:
        return ATREE_OP_LE;
    case ATREE_OP_EQ:
    case ATREE_OP_NE:
    default:
        return op;
    }
}

static const char *type_name(atree_type_t t)
{
    switch (t) {
    case ATREE_TYPE_BOOL:
        return "bool";
    case ATREE_TYPE_INT:
        return "int";
    case ATREE_TYPE_FLOAT:
        return "float";
    case ATREE_TYPE_STRING:
        return "string";
    case ATREE_TYPE_INT_LIST:
        return "int list";
    case ATREE_TYPE_STRING_LIST:
        return "string list";
    default:
        return "?";
    }
}

/* Reports a constructor failure at the predicate's span. */
static void fail_pred(struct parser *p, atree_status_t st, size_t start, atree_attr_id_t attr)
{
    size_t end = p->cur.offset;
    size_t len = end > start ? end - start : 1;
    const struct atree__attr *a = atree__attrs_get(&p->tree->attrs, attr);
    char suffix[96];
    const char *tn = type_name(a->type);
    size_t n;
    if (st == ATREE_ERR_TYPE_MISMATCH) {
        memcpy(suffix, " of type ", 9);
        n = strlen(tn);
        memcpy(suffix + 9, tn, n);
        memcpy(suffix + 9 + n, ": operator or literal not applicable", 37);
        fail_name(p, st, start, len, "attribute ", a->name, a->name_len, suffix);
    } else if (st == ATREE_ERR_INVALID_LITERAL) {
        fail_name(p, st, start, len, "invalid literal for attribute ", a->name, a->name_len,
                  " (empty list, NaN, or not exactly representable)");
    } else {
        fail(p, st, start, len, atree_strerror(st));
    }
}

/* ---- literals and lists ------------------------------------------------- */

struct list_buf {
    struct pi64vec ints;
    struct pstrvec strs;
    struct psizevec lens;
    bool is_string;
};

static void list_buf_init(struct list_buf *b)
{
    pi64vec_init(&b->ints);
    pstrvec_init(&b->strs);
    psizevec_init(&b->lens);
    b->is_string = false;
}

static void list_buf_free(struct parser *p, struct list_buf *b)
{
    uint32_t i;
    for (i = 0; i < b->strs.len; i++) {
        atree__strfree(&p->tmp, b->strs.data[i], b->lens.data[i]);
    }
    pi64vec_free(&p->tmp, &b->ints);
    pstrvec_free(&p->tmp, &b->strs);
    psizevec_free(&p->tmp, &b->lens);
}

/* Copies the current STRING token's unescaped contents into the scratch. */
static char *take_string(struct parser *p, size_t *len)
{
    size_t n = atree__token_string_len(&p->lx, &p->cur);
    char *s = atree__alloc(&p->tmp, n + 1);
    if (s == NULL) {
        fail(p, ATREE_ERR_NOMEM, p->cur.offset, p->cur.length, "out of memory");
        return NULL;
    }
    atree__token_string_copy(&p->lx, &p->cur, s);
    s[n] = '\0';
    *len = n;
    return s;
}

static void list_push_string(struct parser *p, struct list_buf *b)
{
    size_t len = 0;
    char *s = take_string(p, &len);
    if (s == NULL) {
        return;
    }
    if (pstrvec_push(&p->tmp, &b->strs, s) != ATREE_OK) {
        atree__strfree(&p->tmp, s, len);
        fail(p, ATREE_ERR_NOMEM, p->cur.offset, p->cur.length, "out of memory");
        return;
    }
    if (psizevec_push(&p->tmp, &b->lens, len) != ATREE_OK) {
        /* keep strs/lens aligned: drop the string again */
        b->strs.len--;
        atree__strfree(&p->tmp, s, len);
        fail(p, ATREE_ERR_NOMEM, p->cur.offset, p->cur.length, "out of memory");
    }
}

/* list := "[" items "]" | "(" items ")" ; items are all INT or all STRING. */
static void parse_list(struct parser *p, struct list_buf *b)
{
    enum atree__tok close;
    size_t start = p->cur.offset;
    bool first = true;

    if (p->cur.kind == ATREE_TOK_LBRACKET) {
        close = ATREE_TOK_RBRACKET;
    } else if (p->cur.kind == ATREE_TOK_LPAREN) {
        close = ATREE_TOK_RPAREN;
    } else {
        fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length, "expected a list: '[' or '('");
        return;
    }
    advance(p);
    if (p->cur.kind == close) {
        fail(p, ATREE_ERR_INVALID_LITERAL, start, p->cur.offset + 1 - start, "empty list literal");
        return;
    }
    for (;;) {
        if (p->st != ATREE_OK) {
            return;
        }
        if (p->cur.kind == ATREE_TOK_INT) {
            if (!first && b->is_string) {
                fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
                     "mixed list: integer in a string list");
                return;
            }
            if (pi64vec_push(&p->tmp, &b->ints, p->cur.i) != ATREE_OK) {
                fail(p, ATREE_ERR_NOMEM, p->cur.offset, p->cur.length, "out of memory");
                return;
            }
        } else if (p->cur.kind == ATREE_TOK_STRING) {
            if (!first && !b->is_string) {
                fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
                     "mixed list: string in an integer list");
                return;
            }
            b->is_string = true;
            list_push_string(p, b);
            if (p->st != ATREE_OK) {
                return;
            }
        } else if (p->cur.kind == ATREE_TOK_FLOAT) {
            fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
                 "float literals are not allowed in lists");
            return;
        } else {
            fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
                 "expected an integer or string literal in list");
            return;
        }
        first = false;
        advance(p);
        if (p->cur.kind == ATREE_TOK_COMMA) {
            advance(p);
            continue;
        }
        if (p->cur.kind == close) {
            advance(p);
            return;
        }
        fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
             close == ATREE_TOK_RBRACKET ? "expected ',' or ']' in list"
                                         : "expected ',' or ')' in list");
        return;
    }
}

/* ---- predicates --------------------------------------------------------- */

/* Resolves an identifier token to an attribute id or reports unknown. */
static bool attr_of(struct parser *p, const struct atree__token *tok, atree_attr_id_t *id)
{
    const char *name = atree__token_text(&p->lx, tok);
    if (atree__is_keyword(name, tok->length)) {
        fail_name(p, ATREE_ERR_SYNTAX, tok->offset, tok->length, "unexpected keyword ", name,
                  tok->length, "");
        return false;
    }
    *id = atree__attrs_lookup(&p->tree->attrs, name, tok->length);
    if (*id == ATREE_ATTR_INVALID) {
        fail_name(p, ATREE_ERR_UNKNOWN_ATTR, tok->offset, tok->length, "unknown attribute ", name,
                  tok->length, "");
        return false;
    }
    return true;
}

static atree_expr_t *make_leaf(struct parser *p, size_t start, atree_attr_id_t attr,
                               enum atree__pred_kind kind, atree_op_t op,
                               enum atree__vkind operand_kind, const struct atree__value *scalar,
                               const int64_t *ints, size_t nints, const char *const *strs,
                               const size_t *lens, size_t nstrs)
{
    atree_expr_t *e = NULL;
    atree_status_t st;
    if (p->st != ATREE_OK) {
        return NULL;
    }
    st = atree__expr_new_pred(p->tree, attr, kind, op, operand_kind, scalar, ints, nints, strs,
                              lens, nstrs, &e);
    if (st != ATREE_OK) {
        fail_pred(p, st, start, attr);
        return NULL;
    }
    return e;
}

/* The current token is a literal: builds a scalar operand value. */
static void literal_operand(struct parser *p, struct atree__value *v, enum atree__vkind *kind)
{
    if (p->cur.kind == ATREE_TOK_INT) {
        v->kind = ATREE_V_INT;
        v->u.i = p->cur.i;
        *kind = ATREE_V_INT;
    } else if (p->cur.kind == ATREE_TOK_FLOAT) {
        v->kind = ATREE_V_FLOAT;
        v->u.f = p->cur.f;
        *kind = ATREE_V_FLOAT;
    } else {
        v->kind = ATREE_V_STRING;
        v->u.s = 0;
        *kind = ATREE_V_STRING;
    }
}

/* attr <op> literal, with the literal as the current token. */
static atree_expr_t *finish_comparison(struct parser *p, size_t start, atree_attr_id_t attr,
                                       atree_op_t op)
{
    struct atree__value v;
    enum atree__vkind kind;
    atree_expr_t *e;
    if (!is_literal(&p->cur)) {
        fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
             "expected a number or string literal after comparison operator");
        return NULL;
    }
    literal_operand(p, &v, &kind);
    if (kind == ATREE_V_STRING) {
        size_t len = 0;
        char *s = take_string(p, &len);
        const char *strs[1];
        size_t lens[1];
        if (s == NULL) {
            return NULL;
        }
        strs[0] = s;
        lens[0] = len;
        advance(p);
        e = make_leaf(p, start, attr, ATREE_PRED_CMP, op, ATREE_V_STRING, NULL, NULL, 0, strs, lens,
                      1);
        atree__strfree(&p->tmp, s, len);
        return e;
    }
    advance(p);
    return make_leaf(p, start, attr, ATREE_PRED_CMP, op, kind, &v, NULL, 0, NULL, NULL, 0);
}

/* attr <kind> list, with the list opener as the current token. */
static atree_expr_t *finish_list_pred(struct parser *p, size_t start, atree_attr_id_t attr,
                                      enum atree__pred_kind kind)
{
    struct list_buf b;
    atree_expr_t *e = NULL;
    list_buf_init(&b);
    parse_list(p, &b);
    if (p->st == ATREE_OK) {
        if (b.is_string) {
            e = make_leaf(p, start, attr, kind, ATREE_OP_EQ, ATREE_V_STRING_LIST, NULL, NULL, 0,
                          (const char *const *)b.strs.data, b.lens.data, b.strs.len);
        } else {
            e = make_leaf(p, start, attr, kind, ATREE_OP_EQ, ATREE_V_INT_LIST, NULL, b.ints.data,
                          b.ints.len, NULL, NULL, 0);
        }
    }
    list_buf_free(p, &b);
    return e;
}

/* `attr between lo and hi` -> attr >= lo and attr <= hi */
static atree_expr_t *finish_between(struct parser *p, size_t start, atree_attr_id_t attr)
{
    struct atree__value lo;
    struct atree__value hi;
    enum atree__vkind klo;
    enum atree__vkind khi;
    atree_expr_t *c[2];

    if (p->cur.kind != ATREE_TOK_INT && p->cur.kind != ATREE_TOK_FLOAT) {
        fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
             "expected a number after 'between'");
        return NULL;
    }
    literal_operand(p, &lo, &klo);
    advance(p);
    if (!cur_is_word(p, "and")) {
        fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length, "expected 'and' in 'between'");
        return NULL;
    }
    advance(p);
    if (p->cur.kind != ATREE_TOK_INT && p->cur.kind != ATREE_TOK_FLOAT) {
        fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
             "expected a number after 'between ... and'");
        return NULL;
    }
    literal_operand(p, &hi, &khi);
    advance(p);
    c[0] = make_leaf(p, start, attr, ATREE_PRED_CMP, ATREE_OP_GE, klo, &lo, NULL, 0, NULL, NULL, 0);
    c[1] = make_leaf(p, start, attr, ATREE_PRED_CMP, ATREE_OP_LE, khi, &hi, NULL, 0, NULL, NULL, 0);
    if (c[0] == NULL || c[1] == NULL) {
        atree_expr_free(c[0]);
        atree_expr_free(c[1]);
        return NULL;
    }
    {
        atree_expr_t *e = atree_expr_and(c, 2); /* frees c[] on failure */
        if (e == NULL) {
            fail(p, ATREE_ERR_NOMEM, start, 1, "out of memory");
        }
        return e;
    }
}

/* primary starting with an identifier (the current token). */
static atree_expr_t *parse_attr_primary(struct parser *p)
{
    struct atree__token ident = p->cur;
    size_t start = ident.offset;
    atree_attr_id_t attr;
    atree_op_t op;

    if (!attr_of(p, &ident, &attr)) {
        return NULL;
    }
    advance(p);

    if (cmp_token(p->cur.kind, &op)) {
        advance(p);
        return finish_comparison(p, start, attr, op);
    }
    if (cur_is_word(p, "in")) {
        advance(p);
        return finish_list_pred(p, start, attr, ATREE_PRED_IN);
    }
    if (cur_is_word(p, "not") && next_is_word(p, "in")) {
        advance(p);
        advance(p);
        return finish_list_pred(p, start, attr, ATREE_PRED_NOT_IN);
    }
    if ((cur_is_word(p, "one") || cur_is_word(p, "none") || cur_is_word(p, "all")) &&
        next_is_word(p, "of")) {
        enum atree__pred_kind kind = cur_is_word(p, "one") ? ATREE_PRED_ONE_OF
            : cur_is_word(p, "none")                       ? ATREE_PRED_NONE_OF
                                                           : ATREE_PRED_ALL_OF;
        advance(p);
        advance(p);
        return finish_list_pred(p, start, attr, kind);
    }
    if (cur_is_word(p, "is")) {
        bool negated = false;
        enum atree__pred_kind kind;
        advance(p);
        if (cur_is_word(p, "not")) {
            negated = true;
            advance(p);
        }
        if (cur_is_word(p, "null")) {
            kind = negated ? ATREE_PRED_IS_NOT_NULL : ATREE_PRED_IS_NULL;
        } else if (cur_is_word(p, "empty")) {
            kind = negated ? ATREE_PRED_IS_NOT_EMPTY : ATREE_PRED_IS_EMPTY;
        } else {
            fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
                 "expected 'null' or 'empty' after 'is'");
            return NULL;
        }
        advance(p);
        return make_leaf(p, start, attr, kind, ATREE_OP_EQ, ATREE_V_UNDEFINED, NULL, NULL, 0, NULL,
                         NULL, 0);
    }
    if (cur_is_word(p, "between")) {
        advance(p);
        return finish_between(p, start, attr);
    }
    /* bare identifier: boolean attribute */
    return make_leaf(p, start, attr, ATREE_PRED_VAR, ATREE_OP_EQ, ATREE_V_UNDEFINED, NULL, NULL, 0,
                     NULL, NULL, 0);
}

/* primary starting with a literal: `literal op ident` or `literal in ident`. */
static atree_expr_t *parse_literal_primary(struct parser *p)
{
    struct atree__token lit = p->cur;
    size_t start = lit.offset;
    struct atree__value v;
    enum atree__vkind kind;
    char *s = NULL;
    size_t slen = 0;
    atree_op_t op;
    atree_attr_id_t attr;
    atree_expr_t *e = NULL;
    const char *strs[1];
    size_t lens[1];

    literal_operand(p, &v, &kind);
    if (kind == ATREE_V_STRING) {
        s = take_string(p, &slen);
        if (s == NULL) {
            return NULL;
        }
        strs[0] = s;
        lens[0] = slen;
    }
    advance(p);

    if (cmp_token(p->cur.kind, &op)) {
        advance(p);
        if (p->cur.kind != ATREE_TOK_WORD || !attr_of(p, &p->cur, &attr)) {
            if (p->st == ATREE_OK) {
                fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
                     "expected an attribute name after the operator");
            }
            goto done;
        }
        advance(p);
        if (kind == ATREE_V_STRING) {
            e = make_leaf(p, start, attr, ATREE_PRED_CMP, mirror(op), ATREE_V_STRING, NULL, NULL, 0,
                          strs, lens, 1);
        } else {
            e = make_leaf(p, start, attr, ATREE_PRED_CMP, mirror(op), kind, &v, NULL, 0, NULL, NULL,
                          0);
        }
        goto done;
    }
    if (cur_is_word(p, "in")) {
        /* `value in list_attr` == `list_attr one of [value]` */
        advance(p);
        if (p->cur.kind != ATREE_TOK_WORD || !attr_of(p, &p->cur, &attr)) {
            if (p->st == ATREE_OK) {
                fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
                     "expected a list attribute name after 'in'");
            }
            goto done;
        }
        advance(p);
        if (kind == ATREE_V_STRING) {
            e = make_leaf(p, start, attr, ATREE_PRED_ONE_OF, ATREE_OP_EQ, ATREE_V_STRING_LIST, NULL,
                          NULL, 0, strs, lens, 1);
        } else if (kind == ATREE_V_INT) {
            e = make_leaf(p, start, attr, ATREE_PRED_ONE_OF, ATREE_OP_EQ, ATREE_V_INT_LIST, NULL,
                          &v.u.i, 1, NULL, NULL, 0);
        } else {
            fail(p, ATREE_ERR_SYNTAX, lit.offset, lit.length,
                 "float literals cannot be list members");
        }
        goto done;
    }
    fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
         "expected a comparison operator or 'in' after literal");
done:
    if (s != NULL) {
        atree__strfree(&p->tmp, s, slen);
    }
    return e;
}

/* ---- expressions -------------------------------------------------------- */

static atree_expr_t *parse_or(struct parser *p);

static atree_expr_t *parse_primary(struct parser *p)
{
    if (p->st != ATREE_OK) {
        return NULL;
    }
    switch (p->cur.kind) {
    case ATREE_TOK_LPAREN: {
        atree_expr_t *e;
        size_t start = p->cur.offset;
        if (p->depth >= p->tree->max_depth) {
            fail(p, ATREE_ERR_TOO_DEEP, start, 1, "expression nested too deeply");
            return NULL;
        }
        p->depth++;
        advance(p);
        if (p->cur.kind == ATREE_TOK_RPAREN) {
            fail(p, ATREE_ERR_SYNTAX, start, 2, "empty parentheses");
            p->depth--;
            return NULL;
        }
        e = parse_or(p);
        p->depth--;
        if (e == NULL) {
            return NULL;
        }
        if (!expect(p, ATREE_TOK_RPAREN, "expected ')'")) {
            atree_expr_free(e);
            return NULL;
        }
        return e;
    }
    case ATREE_TOK_WORD:
        if (cur_is_word(p, "true") || cur_is_word(p, "false")) {
            atree_expr_t *e = NULL;
            bool value = cur_is_word(p, "true");
            size_t off = p->cur.offset;
            size_t len = p->cur.length;
            advance(p);
            if (atree__expr_new_const(p->tree, value, &e) != ATREE_OK) {
                fail(p, ATREE_ERR_NOMEM, off, len, "out of memory");
            }
            return e;
        }
        return parse_attr_primary(p);
    case ATREE_TOK_INT:
    case ATREE_TOK_FLOAT:
    case ATREE_TOK_STRING:
        return parse_literal_primary(p);
    case ATREE_TOK_END:
        fail(p, ATREE_ERR_SYNTAX, p->cur.offset, 0, "unexpected end of expression");
        return NULL;
    case ATREE_TOK_RPAREN:
    case ATREE_TOK_LBRACKET:
    case ATREE_TOK_RBRACKET:
    case ATREE_TOK_COMMA:
    case ATREE_TOK_LT:
    case ATREE_TOK_LE:
    case ATREE_TOK_GT:
    case ATREE_TOK_GE:
    case ATREE_TOK_EQ:
    case ATREE_TOK_NE:
    case ATREE_TOK_AND:
    case ATREE_TOK_OR:
    case ATREE_TOK_BANG:
    default:
        fail(p, ATREE_ERR_SYNTAX, p->cur.offset, p->cur.length,
             "expected an attribute, literal, 'not', 'true', 'false' or '('");
        return NULL;
    }
}

static atree_expr_t *parse_not(struct parser *p)
{
    if (p->st != ATREE_OK) {
        return NULL;
    }
    if (p->cur.kind == ATREE_TOK_BANG || cur_is_word(p, "not")) {
        atree_expr_t *e;
        size_t start = p->cur.offset;
        if (p->depth >= p->tree->max_depth) {
            fail(p, ATREE_ERR_TOO_DEEP, start, p->cur.length, "expression nested too deeply");
            return NULL;
        }
        advance(p);
        p->depth++;
        e = parse_not(p);
        p->depth--;
        if (e == NULL) {
            return NULL;
        }
        e = atree_expr_not(e);
        if (e == NULL) {
            fail(p, ATREE_ERR_NOMEM, start, 1, "out of memory");
        }
        return e;
    }
    return parse_primary(p);
}

/* Parses `sub ( sep sub )*` into one n-ary node of `kind`. */
static atree_expr_t *parse_chain(struct parser *p, enum atree__expr_kind kind,
                                 atree_expr_t *(*sub)(struct parser *), const char *kw,
                                 enum atree__tok sym)
{
    struct pexprvec parts;
    atree_expr_t *e;
    atree_expr_t *out = NULL;
    uint32_t i;

    e = sub(p);
    if (e == NULL) {
        return NULL;
    }
    if (!(cur_is_word(p, kw) || (sym != ATREE_TOK_END && p->cur.kind == sym))) {
        return e;
    }
    pexprvec_init(&parts);
    if (pexprvec_push(&p->tmp, &parts, e) != ATREE_OK) {
        atree_expr_free(e);
        fail(p, ATREE_ERR_NOMEM, p->cur.offset, p->cur.length, "out of memory");
        return NULL;
    }
    while (p->st == ATREE_OK &&
           (cur_is_word(p, kw) || (sym != ATREE_TOK_END && p->cur.kind == sym))) {
        advance(p);
        e = sub(p);
        if (e == NULL) {
            break;
        }
        if (pexprvec_push(&p->tmp, &parts, e) != ATREE_OK) {
            atree_expr_free(e);
            fail(p, ATREE_ERR_NOMEM, p->cur.offset, p->cur.length, "out of memory");
            break;
        }
    }
    if (p->st == ATREE_OK) {
        /* new_nary takes ownership of the children even on failure */
        if (atree__expr_new_nary(kind, parts.data, parts.len, &out) != ATREE_OK) {
            fail(p, ATREE_ERR_NOMEM, p->cur.offset, p->cur.length, "out of memory");
            out = NULL;
        }
    } else {
        for (i = 0; i < parts.len; i++) {
            atree_expr_free(parts.data[i]);
        }
    }
    pexprvec_free(&p->tmp, &parts);
    return out;
}

static atree_expr_t *parse_and(struct parser *p)
{
    return parse_chain(p, ATREE_EXPR_AND, parse_not, "and", ATREE_TOK_AND);
}

/* xor/xnor are binary and left-associative: a xor b xnor c = (a xor b) xnor c */
static atree_expr_t *parse_xor(struct parser *p)
{
    atree_expr_t *left = parse_and(p);
    while (left != NULL && p->st == ATREE_OK && (cur_is_word(p, "xor") || cur_is_word(p, "xnor"))) {
        bool is_xor = cur_is_word(p, "xor");
        size_t off = p->cur.offset;
        atree_expr_t *right;
        advance(p);
        right = parse_and(p);
        if (right == NULL) {
            atree_expr_free(left);
            return NULL;
        }
        left = is_xor ? atree_expr_xor(left, right) : atree_expr_xnor(left, right);
        if (left == NULL) {
            fail(p, ATREE_ERR_NOMEM, off, 3, "out of memory");
        }
    }
    return left;
}

static atree_expr_t *parse_or(struct parser *p)
{
    return parse_chain(p, ATREE_EXPR_OR, parse_xor, "or", ATREE_TOK_OR);
}

/* ---- public ------------------------------------------------------------- */

atree_status_t atree_expr_parse(const atree_t *tree, const char *text, size_t len,
                                atree_expr_t **out, atree_error_t *err)
{
    struct parser p;
    atree_expr_t *e;

    if (err != NULL) {
        err->status = ATREE_OK;
        err->offset = SIZE_MAX;
        err->length = 0;
        err->message[0] = '\0';
    }
    if (out == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (tree == NULL || text == NULL) {
        atree__error_set(err, ATREE_ERR_INVALID_ARG, SIZE_MAX, 0, "tree or text is NULL");
        return ATREE_ERR_INVALID_ARG;
    }
    if (len == SIZE_MAX) {
        len = strlen(text);
    }

    p.tree = tree;
    atree__lexer_init(&p.lx, text, len);
    p.err = err;
    atree__mem_init(&p.tmp, &tree->mem.a);
    p.depth = 0;
    p.st = ATREE_OK;
    p.cur.kind = ATREE_TOK_END;
    p.cur.offset = 0;
    p.cur.length = 0;

    atree__rdlock(tree);
    advance(&p);
    if (p.st == ATREE_OK && p.cur.kind == ATREE_TOK_END) {
        fail(&p, ATREE_ERR_SYNTAX, 0, 0, "empty expression");
    }
    e = p.st == ATREE_OK ? parse_or(&p) : NULL;
    if (e != NULL && p.st == ATREE_OK && p.cur.kind != ATREE_TOK_END) {
        fail(&p, ATREE_ERR_SYNTAX, p.cur.offset, p.cur.length,
             "unexpected token after end of expression");
    }
    atree__rdunlock(tree);

    if (p.st != ATREE_OK) {
        atree_expr_free(e);
        return p.st;
    }
    if (e == NULL) {
        /* Defensive: every NULL return sets p.st. */
        atree__error_set(err, ATREE_ERR_SYNTAX, 0, 0, "could not parse expression");
        return ATREE_ERR_SYNTAX;
    }
    *out = e;
    return ATREE_OK;
}
