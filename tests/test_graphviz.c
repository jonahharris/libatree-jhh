/*
 * Graphviz export: structure, escaping, cancellation.
 *
 * SPDX-License-Identifier: MIT
 */
#include "test.h"

static const atree_attr_def_t DEFS[] = {
    {"p", ATREE_TYPE_BOOL},
    {"x", ATREE_TYPE_INT},
    {"s", ATREE_TYPE_STRING},
};

struct sink {
    char buf[16384];
    size_t len;
    int cancel_after;
};

static int sink_write(void *ctx, const char *data, size_t len)
{
    struct sink *s = (struct sink *)ctx;
    if (s->cancel_after > 0 && (int)s->len >= s->cancel_after) {
        return 1;
    }
    if (s->len + len >= sizeof s->buf) {
        return 1;
    }
    memcpy(s->buf + s->len, data, len);
    s->len += len;
    s->buf[s->len] = '\0';
    return 0;
}

static int count_sub(const char *hay, const char *needle)
{
    int n = 0;
    const char *p = hay;
    size_t len = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += len;
    }
    return n;
}

TEST(export_structure)
{
    atree_t *t = NULL;
    struct sink s;
    ASSERT_OK(atree_create(NULL, DEFS, 3, &t));
    ASSERT_OK(atree_insert(t, 1, "(p and x = 1) or s = 'a\"b'", SIZE_MAX, NULL));
    ASSERT_OK(atree_insert(t, 2, "p and x = 1", SIZE_MAX, NULL));
    ASSERT_OK(atree_insert(t, 3, "true", SIZE_MAX, NULL));
    s.len = 0;
    s.cancel_after = 0;
    s.buf[0] = '\0';
    ASSERT_OK(atree_to_graphviz(t, sink_write, &s));
    ASSERT_TRUE(strncmp(s.buf, "digraph atree {", 15) == 0);
    ASSERT_TRUE(s.buf[s.len - 2] == '}');
    ASSERT_EQ_I64(count_sub(s.buf, "[label = \""), 5);        /* 3 leaves + AND + OR */
    ASSERT_EQ_I64(count_sub(s.buf, "{ rank = same;"), 3);     /* levels 1..3 */
    ASSERT_EQ_I64(count_sub(s.buf, " -> "), 4);               /* AND: 2 edges, OR: 2 edges */
    ASSERT_EQ_I64(count_sub(s.buf, "label = \"access\""), 1); /* one AND node */
    ASSERT_EQ_I64(count_sub(s.buf, "subscriptions: "), 2);    /* ids 1 and 2 */
    ASSERT_TRUE(strstr(s.buf, "always: 3") != NULL);
    ASSERT_TRUE(strstr(s.buf, "s = \\\"a\\\\\\\"b\\\"") != NULL); /* DOT-escaped DSL quotes */
    ASSERT_TRUE(strstr(s.buf, "AND\\nlevel 2") != NULL);
    ASSERT_TRUE(strstr(s.buf, "OR\\nlevel 3") != NULL);

    /* cancellation from the callback */
    s.len = 0;
    s.cancel_after = 40;
    ASSERT_STATUS(atree_to_graphviz(t, sink_write, &s), ATREE_ERR_CANCELLED);
    ASSERT_STATUS(atree_to_graphviz(NULL, sink_write, &s), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_to_graphviz(t, NULL, &s), ATREE_ERR_INVALID_ARG);

    /* empty tree is a valid graph */
    ASSERT_OK(atree_delete(t, 1));
    ASSERT_OK(atree_delete(t, 2));
    ASSERT_OK(atree_delete(t, 3));
    s.len = 0;
    s.cancel_after = 0;
    ASSERT_OK(atree_to_graphviz(t, sink_write, &s));
    ASSERT_EQ_I64(count_sub(s.buf, "[label"), 0);
    atree_destroy(t);
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(export_structure);
TEST_MAIN_END()
