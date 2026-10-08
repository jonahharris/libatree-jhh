/*
 * Graphviz (DOT) export of the DAG, drawn like the paper's figures: leaves
 * at the bottom, one rank per level, parent -> child edges, access-child
 * edges in bold, subscription ids on their root nodes.
 *
 * SPDX-License-Identifier: MIT
 */
#include "atree_internal.h"
#include "writer.h"

/* DOT label text: escape quotes and backslashes, fold newlines. */
static void write_label(struct atree__writer *w, const char *s, size_t len)
{
    size_t i;
    size_t start = 0;
    for (i = 0; i < len; i++) {
        const char *esc = NULL;
        switch (s[i]) {
        case '"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        case '\n':
            esc = "\\n";
            break;
        default:
            break;
        }
        if (esc != NULL) {
            atree__write(w, s + start, i - start);
            atree__write_cstr(w, esc);
            start = i + 1;
        }
    }
    atree__write(w, s + start, len - start);
}

/* Capture a predicate's DSL text into a small buffer for escaping. */
struct capture {
    char buf[512];
    size_t len;
    int truncated;
};

static int capture_write(void *ctx, const char *data, size_t len)
{
    struct capture *c = (struct capture *)ctx;
    size_t room = sizeof c->buf - 1 - c->len;
    if (len > room) {
        len = room;
        c->truncated = 1;
    }
    memcpy(c->buf + c->len, data, len);
    c->len += len;
    c->buf[c->len] = '\0';
    return 0;
}

static void write_subs(struct atree__writer *w, const atree_t *t, atree__nid id)
{
    uint32_t nsubs;
    const atree_id_t *subs = atree__node_subs(t, id, &nsubs);
    uint32_t i;
    if (nsubs == 0) {
        return;
    }
    atree__write_cstr(w, "\\nsubscriptions: ");
    for (i = 0; i < nsubs && i < 8; i++) {
        if (i > 0) {
            atree__write_cstr(w, ", ");
        }
        atree__write_u64(w, subs[i]);
    }
    if (nsubs > 8) {
        atree__write_cstr(w, ", ...");
    }
}

atree_status_t atree_to_graphviz(const atree_t *t, atree_write_fn fn, void *ctx)
{
    struct atree__writer w;
    uint32_t level;
    uint32_t i;
    uint32_t j;

    if (t == NULL || fn == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    atree__writer_init(&w, fn, ctx);
    atree__rdlock(t);

    atree__write_cstr(
        &w, "digraph atree {\n  rankdir = BT;\n  node [shape = box, fontname = \"Helvetica\"];\n");

    /* nodes, grouped by level so each level shares a rank */
    for (level = 1; level <= t->max_level && !w.cancelled; level++) {
        bool any = false;
        for (i = 0; i < t->nodes.len; i++) {
            const struct atree__node *n = &t->nodes.data[i];
            if (n->kind == ATREE_NODE_FREE || n->level != level) {
                continue;
            }
            any = true;
            atree__write_cstr(&w, "  n");
            atree__write_u64(&w, i);
            atree__write_cstr(&w, " [label = \"");
            if (n->kind == ATREE_NODE_LEAF) {
                struct capture cap;
                struct atree__writer pw;
                cap.len = 0;
                cap.truncated = 0;
                cap.buf[0] = '\0';
                atree__writer_init(&pw, capture_write, &cap);
                atree__pred_print(&t->preds.data[n->pred], &t->attrs, atree__strtab_resolver,
                                  &t->strings, &pw);
                write_label(&w, cap.buf, cap.len);
                if (cap.truncated) {
                    atree__write_cstr(&w, "...");
                }
                atree__write_cstr(&w, "\", style = rounded");
            } else {
                atree__write_cstr(&w, n->kind == ATREE_NODE_AND ? "AND" : "OR");
                atree__write_cstr(&w, "\\nlevel ");
                atree__write_u64(&w, n->level);
                atree__write_cstr(&w, "\"");
            }
            /* subscriptions are appended inside the label, so reopen it */
            if ((n->flags & ATREE_NODE_HAS_SUBS) != 0) {
                atree__write_cstr(&w, ", xlabel = \"");
                write_subs(&w, t, i);
                atree__write_cstr(&w, "\", penwidth = 2");
            }
            atree__write_cstr(&w, "];\n");
        }
        if (any) {
            atree__write_cstr(&w, "  { rank = same;");
            for (i = 0; i < t->nodes.len; i++) {
                const struct atree__node *n = &t->nodes.data[i];
                if (n->kind != ATREE_NODE_FREE && n->level == level) {
                    atree__write_cstr(&w, " n");
                    atree__write_u64(&w, i);
                    atree__write_cstr(&w, ";");
                }
            }
            atree__write_cstr(&w, " }\n");
        }
    }

    /* edges: parent -> child; the access child of an AND node in bold */
    for (i = 0; i < t->nodes.len && !w.cancelled; i++) {
        const struct atree__node *n = &t->nodes.data[i];
        if (n->kind != ATREE_NODE_AND && n->kind != ATREE_NODE_OR) {
            continue;
        }
        for (j = 0; j < n->children.len; j++) {
            atree__write_cstr(&w, "  n");
            atree__write_u64(&w, i);
            atree__write_cstr(&w, " -> n");
            atree__write_u64(&w, n->children.data[j]);
            if (n->children.data[j] == n->access_child) {
                atree__write_cstr(&w, " [penwidth = 2, label = \"access\"]");
            }
            atree__write_cstr(&w, ";\n");
        }
    }
    if (t->always.len > 0 && !w.cancelled) {
        atree__write_cstr(&w, "  always [shape = plaintext, label = \"always: ");
        for (i = 0; i < t->always.len && i < 8; i++) {
            if (i > 0) {
                atree__write_cstr(&w, ", ");
            }
            atree__write_u64(&w, t->always.data[i]);
        }
        atree__write_cstr(&w, t->always.len > 8 ? ", ...\"];\n" : "\"];\n");
    }
    atree__write_cstr(&w, "}\n");
    atree__rdunlock(t);
    return atree__writer_status(&w);
}
