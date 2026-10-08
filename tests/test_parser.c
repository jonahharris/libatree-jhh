/*
 * Tests for the DSL lexer and parser: the Rust crate's parser tests ported,
 * our extensions, diagnostics with offsets, and a print/parse round trip.
 * White-box for normalization and equality (../src/expr.h, ../src/lexer.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/expr.h"
#include "../src/lexer.h"

#include "test.h"
#include "test_alloc.h"

#include "gen.h"

static const atree_attr_def_t DEFS[] = {
    {"private", ATREE_TYPE_BOOL},         {"test", ATREE_TYPE_BOOL},
    {"exchange_id", ATREE_TYPE_INT},      {"price", ATREE_TYPE_FLOAT},
    {"country", ATREE_TYPE_STRING},       {"city", ATREE_TYPE_STRING},
    {"segment_ids", ATREE_TYPE_INT_LIST}, {"deal_ids", ATREE_TYPE_STRING_LIST},
};
#define NDEFS (sizeof DEFS / sizeof DEFS[0])

struct sink {
    char buf[4096];
    size_t len;
};

static int sink_write(void *ctx, const char *data, size_t len)
{
    struct sink *s = (struct sink *)ctx;
    if (s->len + len >= sizeof s->buf) {
        return 1;
    }
    memcpy(s->buf + s->len, data, len);
    s->len += len;
    s->buf[s->len] = '\0';
    return 0;
}

static const char *render(const atree_expr_t *e, struct sink *s)
{
    s->len = 0;
    s->buf[0] = '\0';
    if (atree_expr_print(e, sink_write, s) != ATREE_OK) {
        return "<print failed>";
    }
    return s->buf;
}

/* Parses text and returns the printed form (or the error message). */
static const char *parse_print(const atree_t *t, const char *text, struct sink *s)
{
    atree_expr_t *e = NULL;
    atree_error_t err;
    const char *r;
    if (atree_expr_parse(t, text, SIZE_MAX, &e, &err) != ATREE_OK) {
        s->len = 0;
        snprintf(s->buf, sizeof s->buf, "ERROR(%s@%lu+%lu): %s", atree_strerror(err.status),
                 (unsigned long)err.offset, (unsigned long)err.length, err.message);
        return s->buf;
    }
    r = render(e, s);
    atree_expr_free(e);
    return r;
}

/* Parses two texts and compares their normal forms. */
static int same_normal(const atree_t *t, const char *a, const char *b)
{
    atree_expr_t *ea = NULL;
    atree_expr_t *eb = NULL;
    atree_expr_t *na = NULL;
    atree_expr_t *nb = NULL;
    int eq = 0;
    if (atree_expr_parse(t, a, SIZE_MAX, &ea, NULL) == ATREE_OK &&
        atree_expr_parse(t, b, SIZE_MAX, &eb, NULL) == ATREE_OK &&
        atree__expr_normalize(ea, 64, &na) == ATREE_OK &&
        atree__expr_normalize(eb, 64, &nb) == ATREE_OK) {
        eq = atree__expr_equal(na, nb);
    }
    atree_expr_free(na);
    atree_expr_free(nb);
    atree_expr_free(ea);
    atree_expr_free(eb);
    return eq;
}

/* Expects a parse failure with the given status and offset. */
static int expect_error(const atree_t *t, const char *text, atree_status_t status, size_t offset)
{
    atree_expr_t *e = NULL;
    atree_error_t err;
    atree_status_t st;
    memset(&err, 0x5a, sizeof err);
    st = atree_expr_parse(t, text, SIZE_MAX, &e, &err);
    if (st != status || e != NULL) {
        fprintf(stderr, "  '%s': status %s, expected %s\n", text, atree_strerror(st),
                atree_strerror(status));
        atree_expr_free(e);
        return 1;
    }
    if (err.status != status || err.offset != offset || err.message[0] == '\0' ||
        strlen(err.message) >= ATREE_ERROR_MESSAGE_MAX) {
        fprintf(stderr, "  '%s': err.status=%d offset=%lu (expected %lu) message='%s'\n", text,
                (int)err.status, (unsigned long)err.offset, (unsigned long)offset, err.message);
        return 1;
    }
    return 0;
}

/* ---- lexer -------------------------------------------------------------- */

TEST(lexer_tokens)
{
    struct atree__lexer lx;
    struct atree__token tok;
    atree_error_t err;
    const char *text = " ( ) [ ] , < <= > >= = == <> != && || ! abc_1-x 42 -7 3.5 -0.25 1e3 2.5E-2 "
                       "\"a\\\"b\" 'c' ";
    enum atree__tok expected[] = {
        ATREE_TOK_LPAREN, ATREE_TOK_RPAREN, ATREE_TOK_LBRACKET, ATREE_TOK_RBRACKET,
        ATREE_TOK_COMMA,  ATREE_TOK_LT,     ATREE_TOK_LE,       ATREE_TOK_GT,
        ATREE_TOK_GE,     ATREE_TOK_EQ,     ATREE_TOK_EQ,       ATREE_TOK_NE,
        ATREE_TOK_NE,     ATREE_TOK_AND,    ATREE_TOK_OR,       ATREE_TOK_BANG,
        ATREE_TOK_WORD,   ATREE_TOK_INT,    ATREE_TOK_INT,      ATREE_TOK_FLOAT,
        ATREE_TOK_FLOAT,  ATREE_TOK_FLOAT,  ATREE_TOK_FLOAT,    ATREE_TOK_STRING,
        ATREE_TOK_STRING, ATREE_TOK_END,
    };
    size_t i;
    atree__lexer_init(&lx, text, strlen(text));
    for (i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
        ASSERT_EQ_U64(tok.kind, expected[i]);
        if (tok.kind == ATREE_TOK_WORD) {
            ASSERT_EQ_U64(tok.length, 7);
            ASSERT_TRUE(memcmp(atree__token_text(&lx, &tok), "abc_1-x", 7) == 0);
        }
    }
    atree__lexer_init(&lx, "42 -7 3.5 -0.25 1e3 2.5E-2", 26);
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_EQ_I64(tok.i, 42);
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_EQ_I64(tok.i, -7);
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_TRUE(atree__double_eq(tok.f, 3.5));
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_TRUE(atree__double_eq(tok.f, -0.25));
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_TRUE(atree__double_eq(tok.f, 1000.0));
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_TRUE(atree__double_eq(tok.f, 0.025));

    /* strings: span and unescaping */
    atree__lexer_init(&lx, "\"a\\\"b\\\\c\\n\" 'it''s'", 19);
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_EQ_U64(tok.kind, ATREE_TOK_STRING);
    ASSERT_EQ_U64(tok.offset, 0);
    ASSERT_EQ_U64(tok.length, 11);
    ASSERT_EQ_U64(atree__token_string_len(&lx, &tok), 6);
    {
        char buf[8];
        atree__token_string_copy(&lx, &tok, buf);
        ASSERT_TRUE(memcmp(buf, "a\"b\\c\n", 6) == 0);
    }
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_EQ_U64(tok.kind, ATREE_TOK_STRING); /* 'it' */
    ASSERT_EQ_U64(atree__token_string_len(&lx, &tok), 2);
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_EQ_U64(tok.kind, ATREE_TOK_STRING); /* 's' */

    /* integer extremes and overflow */
    atree__lexer_init(&lx, "9223372036854775807 -9223372036854775808", 40);
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_EQ_I64(tok.i, INT64_MAX);
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_EQ_I64(tok.i, INT64_MIN);
    atree__lexer_init(&lx, "9223372036854775808", 19);
    ASSERT_STATUS(atree__lexer_next(&lx, &tok, &err), ATREE_ERR_INVALID_LITERAL);
    ASSERT_EQ_U64(err.offset, 0);
    ASSERT_EQ_U64(err.length, 19);
    atree__lexer_init(&lx, "-9223372036854775809", 20);
    ASSERT_STATUS(atree__lexer_next(&lx, &tok, &err), ATREE_ERR_INVALID_LITERAL);

    /* lexical errors */
    atree__lexer_init(&lx, "a @ b", 5);
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_STATUS(atree__lexer_next(&lx, &tok, &err), ATREE_ERR_SYNTAX);
    ASSERT_EQ_U64(err.offset, 2);
    atree__lexer_init(&lx, "'abc", 4);
    ASSERT_STATUS(atree__lexer_next(&lx, &tok, &err), ATREE_ERR_SYNTAX);
    ASSERT_EQ_U64(err.offset, 0);
    atree__lexer_init(&lx, "\"abc\\", 5);
    ASSERT_STATUS(atree__lexer_next(&lx, &tok, &err), ATREE_ERR_SYNTAX);
    atree__lexer_init(&lx, "12abc", 5);
    ASSERT_STATUS(atree__lexer_next(&lx, &tok, &err), ATREE_ERR_SYNTAX);
    atree__lexer_init(&lx, "1e400", 5);
    ASSERT_STATUS(atree__lexer_next(&lx, &tok, &err), ATREE_ERR_INVALID_LITERAL);
    atree__lexer_init(&lx, "- 1", 3); /* lone minus */
    ASSERT_STATUS(atree__lexer_next(&lx, &tok, &err), ATREE_ERR_SYNTAX);
    atree__lexer_init(&lx, "a & b", 5);
    ASSERT_OK(atree__lexer_next(&lx, &tok, &err));
    ASSERT_STATUS(atree__lexer_next(&lx, &tok, &err), ATREE_ERR_SYNTAX);
    return 0;
}

/* ---- parser: Rust crate test ports + extensions ------------------------- */

TEST(parse_predicates)
{
    atree_t *t = NULL;
    struct sink s;
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));

    /* comparisons, both orientations (reversed form mirrors the operator) */
    ASSERT_EQ_STR(parse_print(t, "exchange_id < 10", &s), "exchange_id < 10");
    ASSERT_EQ_STR(parse_print(t, "10 < exchange_id", &s), "exchange_id > 10");
    ASSERT_EQ_STR(parse_print(t, "exchange_id <= 10", &s), "exchange_id <= 10");
    ASSERT_EQ_STR(parse_print(t, "10 <= exchange_id", &s), "exchange_id >= 10");
    ASSERT_EQ_STR(parse_print(t, "exchange_id > 10", &s), "exchange_id > 10");
    ASSERT_EQ_STR(parse_print(t, "10 > exchange_id", &s), "exchange_id < 10");
    ASSERT_EQ_STR(parse_print(t, "exchange_id >= 10", &s), "exchange_id >= 10");
    ASSERT_EQ_STR(parse_print(t, "10 >= exchange_id", &s), "exchange_id <= 10");
    ASSERT_EQ_STR(parse_print(t, "price > 1.5", &s), "price > 1.5");
    ASSERT_EQ_STR(parse_print(t, "price > 2", &s), "price > 2.0"); /* promoted */
    ASSERT_EQ_STR(parse_print(t, "price >= -0.5", &s), "price >= -0.5");
    ASSERT_EQ_STR(parse_print(t, "price < 1.0e+20", &s), "price < 1.0e+20");

    /* equality */
    ASSERT_EQ_STR(parse_print(t, "exchange_id = 1", &s), "exchange_id = 1");
    ASSERT_EQ_STR(parse_print(t, "1 = exchange_id", &s), "exchange_id = 1");
    ASSERT_EQ_STR(parse_print(t, "exchange_id <> 1", &s), "exchange_id <> 1");
    ASSERT_EQ_STR(parse_print(t, "exchange_id != 1", &s), "exchange_id <> 1");
    ASSERT_EQ_STR(parse_print(t, "1 <> exchange_id", &s), "exchange_id <> 1");
    ASSERT_EQ_STR(parse_print(t, "country = 'CA'", &s), "country = \"CA\"");
    ASSERT_EQ_STR(parse_print(t, "\"CA\" = country", &s), "country = \"CA\"");
    ASSERT_EQ_STR(parse_print(t, "country <> \"a\\\"b\"", &s), "country <> \"a\\\"b\"");
    ASSERT_EQ_STR(parse_print(t, "price = 1.25", &s), "price = 1.25");

    /* null / empty */
    ASSERT_EQ_STR(parse_print(t, "country is null", &s), "country is null");
    ASSERT_EQ_STR(parse_print(t, "country is not null", &s), "country is not null");
    ASSERT_EQ_STR(parse_print(t, "segment_ids is empty", &s), "segment_ids is empty");
    ASSERT_EQ_STR(parse_print(t, "deal_ids is not empty", &s), "deal_ids is not empty");

    /* set membership, brackets or parentheses, sorted and unique */
    ASSERT_EQ_STR(parse_print(t, "exchange_id in [3, 1, 2, 1]", &s), "exchange_id in [1, 2, 3]");
    ASSERT_EQ_STR(parse_print(t, "exchange_id in (5)", &s), "exchange_id in [5]");
    ASSERT_EQ_STR(parse_print(t, "exchange_id not in (1, 2)", &s), "exchange_id not in [1, 2]");
    ASSERT_EQ_STR(parse_print(t, "country in ['US', \"CA\"]", &s), "country in [\"CA\", \"US\"]");
    ASSERT_EQ_STR(parse_print(t, "country not in ('FR')", &s), "country not in [\"FR\"]");

    /* list operators */
    ASSERT_EQ_STR(parse_print(t, "segment_ids one of [1]", &s), "segment_ids one of [1]");
    ASSERT_EQ_STR(parse_print(t, "segment_ids one of (3, 1, 2)", &s),
                  "segment_ids one of [1, 2, 3]");
    ASSERT_EQ_STR(parse_print(t, "segment_ids none of [2, 1]", &s), "segment_ids none of [1, 2]");
    ASSERT_EQ_STR(parse_print(t, "segment_ids all of [2, 1]", &s), "segment_ids all of [1, 2]");
    ASSERT_EQ_STR(parse_print(t, "deal_ids one of ['deal-2', 'deal-1']", &s),
                  "deal_ids one of [\"deal-1\", \"deal-2\"]");
    ASSERT_EQ_STR(parse_print(t, "deal_ids all of ('x')", &s), "deal_ids all of [\"x\"]");
    ASSERT_EQ_STR(parse_print(t, "deal_ids none of [\"a\", \"b\", \"a\"]", &s),
                  "deal_ids none of [\"a\", \"b\"]");

    /* bool variable, constants */
    ASSERT_EQ_STR(parse_print(t, "private", &s), "private");
    ASSERT_EQ_STR(parse_print(t, "true", &s), "true");
    ASSERT_EQ_STR(parse_print(t, "FALSE", &s), "false");

    /* extensions: between, value in list_attr, case-insensitive keywords */
    ASSERT_EQ_STR(parse_print(t, "exchange_id between 1 and 5", &s),
                  "exchange_id >= 1 and exchange_id <= 5");
    ASSERT_EQ_STR(parse_print(t, "price BETWEEN 1 AND 2.5", &s), "price >= 1.0 and price <= 2.5");
    ASSERT_EQ_STR(parse_print(t, "3 in segment_ids", &s), "segment_ids one of [3]");
    ASSERT_EQ_STR(parse_print(t, "'deal-1' IN deal_ids", &s), "deal_ids one of [\"deal-1\"]");
    ASSERT_EQ_STR(parse_print(t, "Country IS NOT NULL", &s),
                  "ERROR(unknown attribute@0+7): unknown attribute 'Country'");
    ASSERT_EQ_STR(parse_print(t, "country Is Not Null", &s), "country is not null");
    ASSERT_EQ_STR(parse_print(t, "segment_ids ONE OF [1]", &s), "segment_ids one of [1]");

    atree_destroy(t);
    return 0;
}

TEST(parse_connectives_and_precedence)
{
    atree_t *t = NULL;
    struct sink s;
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));

    ASSERT_EQ_STR(parse_print(t, "private and test", &s), "private and test");
    ASSERT_EQ_STR(parse_print(t, "private && test", &s), "private and test");
    ASSERT_EQ_STR(parse_print(t, "private or test", &s), "private or test");
    ASSERT_EQ_STR(parse_print(t, "private || test", &s), "private or test");
    ASSERT_EQ_STR(parse_print(t, "not private", &s), "not private");
    ASSERT_EQ_STR(parse_print(t, "!private", &s), "not private");
    ASSERT_EQ_STR(parse_print(t, "not not private", &s), "not not private");
    ASSERT_EQ_STR(parse_print(t, "private xor test", &s), "private xor test");
    ASSERT_EQ_STR(parse_print(t, "private xnor test", &s), "private xnor test");

    /* chains are n-ary (flat), odd and even counts */
    ASSERT_EQ_STR(parse_print(t, "private and test and exchange_id = 1 and price > 1.5", &s),
                  "private and test and exchange_id = 1 and price > 1.5");
    ASSERT_EQ_STR(parse_print(t, "private or test or exchange_id = 1", &s),
                  "private or test or exchange_id = 1");

    /* precedence: not > and > xor/xnor > or */
    ASSERT_EQ_STR(parse_print(t, "private or test and exchange_id = 1", &s),
                  "private or (test and exchange_id = 1)");
    ASSERT_EQ_STR(parse_print(t, "private and test or exchange_id = 1", &s),
                  "(private and test) or exchange_id = 1");
    ASSERT_EQ_STR(parse_print(t, "not private and test", &s), "not private and test");
    ASSERT_EQ_STR(parse_print(t, "not (private and test)", &s), "not (private and test)");
    ASSERT_EQ_STR(parse_print(t, "private and test xor exchange_id = 1 or price > 1.5", &s),
                  "((private and test) xor exchange_id = 1) or price > 1.5");
    ASSERT_EQ_STR(parse_print(t, "private xor test xnor exchange_id = 1", &s),
                  "(private xor test) xnor exchange_id = 1"); /* left-assoc */
    ASSERT_EQ_STR(parse_print(t, "not exchange_id = 1", &s), "not exchange_id = 1");
    ASSERT_EQ_STR(parse_print(t, "not exchange_id in [1, 2] and private", &s),
                  "not exchange_id in [1, 2] and private");

    /* parentheses, nesting, between inside a chain */
    ASSERT_EQ_STR(parse_print(t, "(private)", &s), "private");
    ASSERT_EQ_STR(parse_print(t, "((((private))))", &s), "private");
    ASSERT_EQ_STR(parse_print(t, "(private or test) and (exchange_id = 1 or price > 1.5)", &s),
                  "(private or test) and (exchange_id = 1 or price > 1.5)");
    ASSERT_EQ_STR(parse_print(t, "exchange_id between 1 and 5 and private", &s),
                  "(exchange_id >= 1 and exchange_id <= 5) and private");
    ASSERT_EQ_STR(parse_print(t, "a_b-c = 1", &s),
                  "ERROR(unknown attribute@0+5): unknown attribute 'a_b-c'");

    /* the Rust crate's complex examples */
    ASSERT_EQ_STR(
        parse_print(
            t,
            "(exchange_id = 1 and deal_ids one of ['deal-1', 'deal-2']) and segment_ids one of "
            "[1, 2, 3] and ((country = 'CA' and city in ['QC']) or (country = 'US' and city in "
            "['AZ']))",
            &s),
        "(exchange_id = 1 and deal_ids one of [\"deal-1\", \"deal-2\"]) and segment_ids one of [1, "
        "2, "
        "3] and ((country = \"CA\" and city in [\"QC\"]) or (country = \"US\" and city in "
        "[\"AZ\"]))");
    ASSERT_EQ_STR(
        parse_print(t,
                    "exchange_id = 1 and not private and deal_ids one of [\"deal-1\", "
                    "\"deal-2\"] and segment_ids one of [1, 2, 3] and country = 'CA' and "
                    "city in ['QC'] or country = 'US' and city in ['AZ']",
                    &s),
        "(exchange_id = 1 and not private and deal_ids one of [\"deal-1\", \"deal-2\"] and "
        "segment_ids one of [1, 2, 3] and country = \"CA\" and city in [\"QC\"]) or "
        "(country = \"US\" and city in [\"AZ\"])");

    /* whitespace and length handling */
    ASSERT_EQ_STR(parse_print(t, "  \t private\n and\r\n test  ", &s), "private and test");
    {
        atree_expr_t *e = NULL;
        ASSERT_OK(
            atree_expr_parse(t, "private and test garbage", 16, &e, NULL)); /* len-delimited */
        ASSERT_EQ_STR(render(e, &s), "private and test");
        atree_expr_free(e);
    }

    /* equivalences via normalization */
    ASSERT_TRUE(same_normal(t, "private and (test and exchange_id = 1)",
                            "exchange_id = 1 and test and private"));
    ASSERT_TRUE(same_normal(t, "not (private or test)", "not private and not test"));
    ASSERT_TRUE(
        same_normal(t, "private xor test", "(private and not test) or (not private and test)"));
    ASSERT_TRUE(same_normal(t, "not (private xor test)", "private xnor test"));
    ASSERT_TRUE(
        same_normal(t, "exchange_id between 1 and 5", "1 <= exchange_id and exchange_id <= 5"));
    ASSERT_TRUE(same_normal(t, "3 in segment_ids", "segment_ids one of [3]"));
    ASSERT_FALSE(same_normal(t, "private and test", "private or test"));

    atree_destroy(t);
    return 0;
}

TEST(parse_errors_with_positions)
{
    atree_t *t = NULL;
    int bad = 0;
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));

    /*                              text                                   status offset */
    bad += expect_error(t, "", ATREE_ERR_SYNTAX, 0);
    bad += expect_error(t, "   ", ATREE_ERR_SYNTAX, 0);
    bad += expect_error(t, "invalid in (1, 2, 3 and", ATREE_ERR_UNKNOWN_ATTR, 0);
    bad += expect_error(t, "exchange_id in (1, 2, 3 and", ATREE_ERR_SYNTAX, 24);
    bad += expect_error(t, "exchange_id in []", ATREE_ERR_INVALID_LITERAL, 15);
    bad += expect_error(t, "exchange_id in ()", ATREE_ERR_INVALID_LITERAL, 15);
    bad += expect_error(t, "exchange_id in [1, 'a']", ATREE_ERR_SYNTAX, 19);
    bad += expect_error(t, "exchange_id in [1.5]", ATREE_ERR_SYNTAX, 16);
    bad += expect_error(t, "exchange_id in [1 2]", ATREE_ERR_SYNTAX, 18);
    bad += expect_error(t, "exchange_id in [1, 2)", ATREE_ERR_SYNTAX, 20);
    bad += expect_error(t, "()", ATREE_ERR_SYNTAX, 0);
    bad += expect_error(t, "(private", ATREE_ERR_SYNTAX, 8);
    bad += expect_error(t, "private)", ATREE_ERR_SYNTAX, 7);
    bad += expect_error(t, "private and", ATREE_ERR_SYNTAX, 11);
    bad += expect_error(t, "and private", ATREE_ERR_SYNTAX, 0);
    bad += expect_error(t, "private test", ATREE_ERR_SYNTAX, 8);
    bad += expect_error(t, "exchange_id =", ATREE_ERR_SYNTAX, 13);
    bad += expect_error(t, "exchange_id = private", ATREE_ERR_SYNTAX, 14);
    bad += expect_error(t, "exchange_id is", ATREE_ERR_SYNTAX, 14);
    bad += expect_error(t, "exchange_id is maybe", ATREE_ERR_SYNTAX, 15);
    bad += expect_error(t, "exchange_id between 1 or 2", ATREE_ERR_SYNTAX, 22);
    bad += expect_error(t, "exchange_id between 'a' and 2", ATREE_ERR_SYNTAX, 20);
    bad += expect_error(t, "1 + exchange_id", ATREE_ERR_SYNTAX, 2);
    bad += expect_error(t, "1 = 2", ATREE_ERR_SYNTAX, 4);
    bad += expect_error(t, "'x' in exchange_id", ATREE_ERR_TYPE_MISMATCH, 0);
    bad += expect_error(t, "1.5 in segment_ids", ATREE_ERR_SYNTAX, 0);
    bad += expect_error(t, "nope = 1", ATREE_ERR_UNKNOWN_ATTR, 0);
    bad += expect_error(t, "private and nope", ATREE_ERR_UNKNOWN_ATTR, 12);
    bad += expect_error(t, "exchange_id = 'x'", ATREE_ERR_TYPE_MISMATCH, 0);
    bad += expect_error(t, "exchange_id = 1.5", ATREE_ERR_TYPE_MISMATCH, 0);
    bad += expect_error(t, "country < 'a'", ATREE_ERR_TYPE_MISMATCH, 0);
    bad += expect_error(t, "private = 1", ATREE_ERR_TYPE_MISMATCH, 0);
    bad += expect_error(t, "exchange_id", ATREE_ERR_TYPE_MISMATCH, 0);
    bad += expect_error(t, "segment_ids in [1]", ATREE_ERR_TYPE_MISMATCH, 0);
    bad += expect_error(t, "exchange_id one of [1]", ATREE_ERR_TYPE_MISMATCH, 0);
    bad += expect_error(t, "segment_ids one of ['a']", ATREE_ERR_TYPE_MISMATCH, 0);
    bad += expect_error(t, "segment_ids is null", ATREE_ERR_TYPE_MISMATCH, 0);
    bad += expect_error(t, "country is empty", ATREE_ERR_TYPE_MISMATCH, 0);
    bad += expect_error(t, "private and country = 'a' and city < 2", ATREE_ERR_TYPE_MISMATCH, 30);
    bad += expect_error(t, "price > 9007199254740993", ATREE_ERR_INVALID_LITERAL, 0);
    bad += expect_error(t, "exchange_id = 99999999999999999999", ATREE_ERR_INVALID_LITERAL, 14);
    bad += expect_error(t, "price = 1e999", ATREE_ERR_INVALID_LITERAL, 8);
    bad += expect_error(t, "price = 1e-400", ATREE_ERR_INVALID_LITERAL, 8); /* underflow */
    bad += expect_error(t, "price = 0.000001e-400", ATREE_ERR_INVALID_LITERAL, 8);
    bad += expect_error(t, "country = 'abc", ATREE_ERR_SYNTAX, 10);
    bad += expect_error(t, "country = \"abc\\", ATREE_ERR_SYNTAX, 10);
    bad += expect_error(t, "private @ test", ATREE_ERR_SYNTAX, 8);
    bad += expect_error(t, "private & test", ATREE_ERR_SYNTAX, 8);
    bad += expect_error(t, "and = 1", ATREE_ERR_SYNTAX, 0);
    bad += expect_error(t, "exchange_id = 12abc", ATREE_ERR_SYNTAX, 14);
    bad += expect_error(t, "not", ATREE_ERR_SYNTAX, 3);
    ASSERT_EQ_I64(bad, 0);

    /* nesting limit */
    {
        atree_config_t cfg;
        atree_t *small = NULL;
        atree_config_init(&cfg);
        cfg.max_depth = 4;
        ASSERT_OK(atree_create(&cfg, DEFS, NDEFS, &small));
        bad += expect_error(small, "(((((private)))))", ATREE_ERR_TOO_DEEP, 4);
        bad += expect_error(small, "not not not not not private", ATREE_ERR_TOO_DEEP, 16);
        {
            struct sink s;
            ASSERT_EQ_STR(parse_print(small, "((((private))))", &s), "private");
            ASSERT_EQ_STR(parse_print(small, "not not not not private", &s),
                          "not not not not private");
        }
        atree_destroy(small);
        ASSERT_EQ_I64(bad, 0);
    }

    /* argument errors and err reset */
    {
        atree_expr_t *e = NULL;
        atree_error_t err;
        memset(&err, 0x5a, sizeof err);
        ASSERT_STATUS(atree_expr_parse(NULL, "private", SIZE_MAX, &e, &err), ATREE_ERR_INVALID_ARG);
        ASSERT_STATUS(atree_expr_parse(t, NULL, SIZE_MAX, &e, &err), ATREE_ERR_INVALID_ARG);
        ASSERT_STATUS(atree_expr_parse(t, "private", SIZE_MAX, NULL, &err), ATREE_ERR_INVALID_ARG);
        ASSERT_OK(atree_expr_parse(t, "private", SIZE_MAX, &e, &err));
        ASSERT_EQ_U64(err.status, ATREE_OK);
        ASSERT_EQ_U64(err.offset, SIZE_MAX);
        ASSERT_EQ_STR(err.message, "");
        atree_expr_free(e);
        ASSERT_OK(atree_expr_parse(t, "private", SIZE_MAX, &e, NULL)); /* err may be NULL */
        atree_expr_free(e);
    }
    atree_destroy(t);
    return 0;
}

/* ---- round trip --------------------------------------------------------- */

TEST(print_parse_round_trip)
{
    struct gen g;
    struct sink s;
    uint64_t seed = gen_seed_from_env(20260306);
    int failures = 0;
    int i;
    ASSERT_OK(gen_init(&g, NULL, seed, 6));
    for (i = 0; i < 500 && failures == 0; i++) {
        atree_expr_t *e = gen_expr(&g);
        atree_expr_t *back = NULL;
        atree_expr_t *n1 = NULL;
        atree_expr_t *n2 = NULL;
        atree_error_t err;
        const char *text;
        ASSERT_NOT_NULL(e);
        text = render(e, &s);
        if (atree_expr_parse(g.tree, text, SIZE_MAX, &back, &err) != ATREE_OK) {
            fprintf(stderr, "seed %llu #%d: reparse failed @%lu: %s\n  text: %s\n",
                    (unsigned long long)seed, i, (unsigned long)err.offset, err.message, text);
            failures++;
        } else {
            /* Printed text re-parses to the same structure... */
            if (!atree__expr_equal(e, back)) {
                /* ...up to the normal form at least (the printer flattens nothing,
                 * but n-ary nodes and binary xor chains should be identical). */
                ASSERT_OK(atree__expr_normalize(e, 64, &n1));
                ASSERT_OK(atree__expr_normalize(back, 64, &n2));
                if (!atree__expr_equal(n1, n2)) {
                    fprintf(stderr, "seed %llu #%d: round trip differs\n  text: %s\n",
                            (unsigned long long)seed, i, text);
                    gen_dump_expr("  reparsed", back);
                    failures++;
                }
                atree_expr_free(n1);
                atree_expr_free(n2);
            }
            atree_expr_free(back);
        }
        atree_expr_free(e);
    }
    gen_free(&g);
    ASSERT_EQ_I64(failures, 0);
    return 0;
}

/* ---- allocation failures ------------------------------------------------ */

TEST(parse_allocation_failures_do_not_leak)
{
    struct test_alloc ta;
    atree_config_t cfg;
    atree_t *t = NULL;
    const char *text = "(exchange_id = 1 and deal_ids one of ['deal-1', 'deal-2', 'deal-3']) and "
                       "segment_ids one of [1, 2, 3] and ((country = 'CA' and city in ['QC']) or "
                       "(country = 'US' and not city in ['AZ'])) xor price between 1 and 2.5";
    size_t k;
    size_t total;
    int leaks = 0;

    test_alloc_init(&ta);
    atree_config_init(&cfg);
    cfg.allocator = &ta.a;
    ASSERT_OK(atree_create(&cfg, DEFS, NDEFS, &t));
    {
        size_t base = ta.attempts;
        atree_expr_t *e = NULL;
        ASSERT_OK(atree_expr_parse(t, text, SIZE_MAX, &e, NULL));
        total = ta.attempts - base;
        atree_expr_free(e);
    }
    ASSERT_TRUE(total > 20);
    for (k = 1; k <= total; k++) {
        atree_expr_t *e = NULL;
        atree_error_t err;
        size_t live = ta.live;
        atree_status_t st;
        ta.fail_at = ta.attempts + k;
        st = atree_expr_parse(t, text, SIZE_MAX, &e, &err);
        ta.fail_at = 0;
        if (st == ATREE_OK) {
            atree_expr_free(e);
        } else {
            if (st != ATREE_ERR_NOMEM || e != NULL || err.status != ATREE_ERR_NOMEM) {
                fprintf(stderr, "k=%lu: status %s\n", (unsigned long)k, atree_strerror(st));
                leaks++;
            }
        }
        if (ta.live != live) {
            fprintf(stderr, "k=%lu: leak\n", (unsigned long)k);
            leaks++;
        }
    }
    atree_destroy(t);
    ASSERT_TRUE(test_alloc_clean(&ta));
    ASSERT_EQ_I64(leaks, 0);
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(lexer_tokens);
RUN_TEST(parse_predicates);
RUN_TEST(parse_connectives_and_precedence);
RUN_TEST(parse_errors_with_positions);
RUN_TEST(print_parse_round_trip);
RUN_TEST(parse_allocation_failures_do_not_leak);
TEST_MAIN_END()
