/*
 * atree_shell: an interactive shell, a small server and a client for
 * playing with libatree. Commands are lines of text; the same interpreter
 * serves stdin, TCP clients and Unix-socket clients.
 *
 *   atree_shell [--defs F] [--exprs F] [--events F]        read commands from stdin
 *   atree_shell --listen PORT | --listen PATH [...]        serve many clients (TCP or Unix)
 *   atree_shell --connect HOST PORT | --connect PATH       talk to a server
 *
 * Commands (case-insensitive keyword, one per line):
 *
 *   DEFINE name type         add an attribute (bool|int|float|string|int_list|string_list)
 *   CREATE                   (re)build the tree over the defined attributes; drops subscriptions
 *   SUBSCRIBE id expr        register a continuous query; id is an unsigned integer
 *   UNSUBSCRIBE id
 *   EVENT attr=value;...     ingest an event: MATCH line, TIME line, STATS line
 *   PARSE expr               echo the expression as the parser understood it
 *   LOAD DEFS|EXPRS|EVENTS F load a file in the bench_file format
 *   COUNT | STATS | VALIDATE | DOT | HELP | QUIT
 *
 * Every command answers with zero or more data lines and a final line that
 * starts with OK or ERR. In server mode a client that subscribed an id is
 * sent an asynchronous "NOTIFY id event" line whenever another client's
 * event matches it; a client's subscriptions end with its connection.
 *
 * POSIX only (sockets, select); the library itself has no such dependency.
 *
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "atree.h"

#include "../bench/bench_clock.h"
#include "../bench/bench_format.h"

#define MAX_DEFS 4096
#define MAX_CLIENTS 64
#define MATCH_IDS_SHOWN 100

#if defined(__GNUC__)
#define SHELL_PRINTF(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define SHELL_PRINTF(fmt_index, first_arg)
#endif

/* ---- buffered output on a file descriptor --------------------------------- */

struct wbuf {
    int fd;
    size_t n;
    char b[8192];
};

static void wb_flush(struct wbuf *w)
{
    size_t off = 0;
    while (off < w->n) {
        ssize_t k = write(w->fd, w->b + off, w->n - off);
        if (k <= 0) {
            break; /* a closed peer; the caller notices on its next read */
        }
        off += (size_t)k;
    }
    w->n = 0;
}

static void wb_write(struct wbuf *w, const char *data, size_t len)
{
    while (len > 0) {
        size_t room = sizeof w->b - w->n;
        size_t k = len < room ? len : room;
        memcpy(w->b + w->n, data, k);
        w->n += k;
        data += k;
        len -= k;
        if (w->n == sizeof w->b) {
            wb_flush(w);
        }
    }
}

static int wb_write_cb(void *ctx, const char *data, size_t len)
{
    wb_write((struct wbuf *)ctx, data, len);
    return 0;
}

static void wb_printf(struct wbuf *w, const char *fmt, ...) SHELL_PRINTF(2, 3);

static void wb_printf(struct wbuf *w, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) {
        wb_write(w, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
    }
}

/* ---- state ---------------------------------------------------------------- */

struct client {
    int fd;
    size_t len;    /* bytes of a partial line in buf */
    char *buf;     /* MAX_LINE */
    uint64_t *ids; /* sorted subscription ids owned by this connection */
    size_t nids;
    size_t capids;
};

struct shell {
    atree_t *tree;
    atree_event_t *ev;
    atree_report_t *rep;
    char *def_names[MAX_DEFS];
    atree_type_t def_types[MAX_DEFS];
    size_t ndefs;
    struct client clients[MAX_CLIENTS];
    size_t nclients;
    int quit;
};

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
        return "int_list";
    case ATREE_TYPE_STRING_LIST:
        return "string_list";
    default:
        return "?";
    }
}

static void tree_drop(struct shell *sh)
{
    size_t i;
    atree_report_destroy(sh->rep);
    atree_event_destroy(sh->ev);
    atree_destroy(sh->tree);
    sh->rep = NULL;
    sh->ev = NULL;
    sh->tree = NULL;
    for (i = 0; i < sh->nclients; i++) {
        sh->clients[i].nids = 0;
    }
}

/* Builds the tree over the defined attributes. */
static atree_status_t tree_create(struct shell *sh)
{
    atree_attr_def_t defs[MAX_DEFS];
    size_t i;
    atree_status_t st;
    if (sh->ndefs == 0) {
        return ATREE_ERR_INVALID_ARG;
    }
    for (i = 0; i < sh->ndefs; i++) {
        defs[i].name = sh->def_names[i];
        defs[i].type = sh->def_types[i];
    }
    tree_drop(sh);
    st = atree_create(NULL, defs, sh->ndefs, &sh->tree);
    if (st == ATREE_OK) {
        st = atree_event_create(sh->tree, &sh->ev);
    }
    if (st == ATREE_OK) {
        st = atree_report_create(sh->tree, &sh->rep);
    }
    if (st != ATREE_OK) {
        tree_drop(sh);
    }
    return st;
}

static atree_status_t ensure_tree(struct shell *sh)
{
    return sh->tree != NULL ? ATREE_OK : tree_create(sh);
}

/* ---- per-client subscription ids (sorted) --------------------------------- */

static size_t ids_lower_bound(const uint64_t *v, size_t n, uint64_t id)
{
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v[mid] < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static int ids_add(struct client *c, uint64_t id)
{
    size_t pos = ids_lower_bound(c->ids, c->nids, id);
    if (pos < c->nids && c->ids[pos] == id) {
        return 0;
    }
    if (c->nids == c->capids) {
        size_t cap = c->capids == 0 ? 16 : c->capids * 2;
        uint64_t *v = (uint64_t *)realloc(c->ids, cap * sizeof *v);
        if (v == NULL) {
            return 1;
        }
        c->ids = v;
        c->capids = cap;
    }
    memmove(c->ids + pos + 1, c->ids + pos, (c->nids - pos) * sizeof *c->ids);
    c->ids[pos] = id;
    c->nids++;
    return 0;
}

static void ids_remove(struct client *c, uint64_t id)
{
    size_t pos = ids_lower_bound(c->ids, c->nids, id);
    if (pos < c->nids && c->ids[pos] == id) {
        memmove(c->ids + pos, c->ids + pos + 1, (c->nids - pos - 1) * sizeof *c->ids);
        c->nids--;
    }
}

/* ---- command helpers ------------------------------------------------------ */

static int keyword_is(const char *word, const char *kw)
{
    for (; *word != '\0' && *kw != '\0'; word++, kw++) {
        int a = (unsigned char)*word;
        int b = (unsigned char)*kw;
        if (a >= 'a' && a <= 'z') {
            a -= 'a' - 'A';
        }
        if (a != b) {
            return 0;
        }
    }
    return *word == '\0' && *kw == '\0';
}

/* Splits off the first space-delimited word; returns the rest (trimmed). */
static char *split_word(char *line, char **word)
{
    char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    *word = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p != '\0') {
        *p++ = '\0';
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static void reply_err(struct wbuf *w, atree_status_t st, const atree_error_t *err)
{
    if (err != NULL && err->message[0] != '\0') {
        if (err->offset != SIZE_MAX) {
            wb_printf(w, "ERR %s: %s (at offset %lu)\n", atree_strerror(st), err->message,
                      (unsigned long)err->offset);
        } else {
            wb_printf(w, "ERR %s: %s\n", atree_strerror(st), err->message);
        }
    } else {
        wb_printf(w, "ERR %s\n", atree_strerror(st));
    }
}

static int parse_id(const char *s, uint64_t *id)
{
    char *end = NULL;
    if (*s == '\0' || *s == '-') {
        return 1;
    }
    errno = 0;
    *id = strtoull(s, &end, 10);
    return errno != 0 || end == s || (*end != '\0' && *end != ' ' && *end != '\t');
}

static void cmd_help(struct wbuf *w)
{
    wb_write(
        w,
        "DEFINE name type         add an attribute: bool int float string int_list string_list\n"
        "CREATE                   (re)build the tree over the defined attributes\n"
        "SUBSCRIBE id expr        continuous query, e.g. SUBSCRIBE 7 price > 10 and country in "
        "['US','CA']\n"
        "UNSUBSCRIBE id\n"
        "EVENT a=v;b=v;...        values: true false 12 1.5 \"text\" [1,2] [\"a\",\"b\"]\n"
        "PARSE expr               echo the expression as parsed\n"
        "LOAD DEFS|EXPRS|EVENTS F load a bench_file-format file\n"
        "COUNT STATS VALIDATE DOT HELP QUIT\n"
        "OK\n",
        0);
}

static int cmd_define(struct shell *sh, char *args, struct wbuf *w)
{
    char *name;
    char *type;
    atree_type_t ty;
    char *rest = split_word(args, &name);
    split_word(rest, &type);
    if (*name == '\0' || *type == '\0' || type_of(type, &ty) != 0) {
        wb_printf(w, "ERR usage: DEFINE name bool|int|float|string|int_list|string_list\n");
        return 0;
    }
    if (sh->ndefs == MAX_DEFS) {
        wb_printf(w, "ERR too many attributes\n");
        return 0;
    }
    sh->def_names[sh->ndefs] = strdup(name);
    if (sh->def_names[sh->ndefs] == NULL) {
        wb_printf(w, "ERR out of memory\n");
        return 0;
    }
    sh->def_types[sh->ndefs] = ty;
    sh->ndefs++;
    wb_printf(w, "OK %s %s%s\n", name, type_name(ty),
              sh->tree != NULL ? " (CREATE to apply; subscriptions will be dropped)" : "");
    return 0;
}

static void cmd_create(struct shell *sh, struct wbuf *w)
{
    atree_status_t st = tree_create(sh);
    if (st != ATREE_OK) {
        wb_printf(w, "ERR %s%s\n", atree_strerror(st),
                  sh->ndefs == 0 ? ": define attributes first" : "");
        return;
    }
    wb_printf(w, "OK tree over %lu attributes\n", (unsigned long)sh->ndefs);
}

static void cmd_subscribe(struct shell *sh, struct client *c, char *args, struct wbuf *w)
{
    char *idtext;
    char *expr = split_word(args, &idtext);
    uint64_t id;
    atree_error_t err;
    atree_status_t st;
    if (parse_id(idtext, &id) != 0 || *expr == '\0') {
        wb_printf(w, "ERR usage: SUBSCRIBE id expression\n");
        return;
    }
    st = ensure_tree(sh);
    if (st != ATREE_OK) {
        wb_printf(w, "ERR %s: define attributes first\n", atree_strerror(st));
        return;
    }
    st = atree_insert(sh->tree, id, expr, SIZE_MAX, &err);
    if (st != ATREE_OK) {
        reply_err(w, st, &err);
        return;
    }
    if (c != NULL && ids_add(c, id) != 0) {
        atree_delete(sh->tree, id);
        wb_printf(w, "ERR out of memory\n");
        return;
    }
    wb_printf(w, "OK subscribed %llu\n", (unsigned long long)id);
}

static void cmd_unsubscribe(struct shell *sh, char *args, struct wbuf *w)
{
    uint64_t id;
    atree_status_t st;
    size_t i;
    if (parse_id(args, &id) != 0) {
        wb_printf(w, "ERR usage: UNSUBSCRIBE id\n");
        return;
    }
    if (sh->tree == NULL) {
        wb_printf(w, "ERR %s\n", atree_strerror(ATREE_ERR_NOT_FOUND));
        return;
    }
    st = atree_delete(sh->tree, id);
    if (st != ATREE_OK) {
        wb_printf(w, "ERR %s\n", atree_strerror(st));
        return;
    }
    for (i = 0; i < sh->nclients; i++) {
        ids_remove(&sh->clients[i], id);
    }
    wb_printf(w, "OK unsubscribed %llu\n", (unsigned long long)id);
}

/* Fills sh->ev from "a=v;b=v" text (modified in place). Returns the bad item or NULL. */
static const char *event_fill(struct shell *sh, char *text)
{
    char *item = text;
    atree_event_clear(sh->ev);
    while (item != NULL && *item != '\0') {
        char *semi = strchr(item, ';');
        if (semi != NULL) {
            *semi = '\0';
        }
        while (*item == ' ') {
            item++;
        }
        if (*item != '\0' && set_item(sh->tree, sh->ev, item) != 0) {
            return item;
        }
        item = semi != NULL ? semi + 1 : NULL;
    }
    return NULL;
}

/* Sends NOTIFY lines to every other client whose ids are among the matches. */
static void notify_clients(struct shell *sh, const struct client *from, const char *event_text)
{
    const atree_id_t *m = atree_report_matches(sh->rep);
    size_t nm = atree_report_count(sh->rep);
    size_t k;
    for (k = 0; k < sh->nclients; k++) {
        struct client *c = &sh->clients[k];
        struct wbuf w;
        size_t i = 0;
        size_t j = 0;
        if (c == from || c->nids == 0 || nm == 0) {
            continue;
        }
        w.fd = c->fd;
        w.n = 0;
        while (i < nm && j < c->nids) {
            if (m[i] < c->ids[j]) {
                i++;
            } else if (m[i] > c->ids[j]) {
                j++;
            } else {
                wb_printf(&w, "NOTIFY %llu %s\n", (unsigned long long)m[i], event_text);
                i++;
                j++;
            }
        }
        wb_flush(&w);
    }
}

static void cmd_event(struct shell *sh, struct client *c, char *args, struct wbuf *w, int verbose)
{
    char *copy;
    const char *bad;
    uint64_t t0;
    uint64_t t1;
    atree_status_t st;
    atree_report_stats_t rs;
    size_t n;
    size_t i;
    if (*args == '\0') {
        wb_printf(w, "ERR usage: EVENT attr=value;attr=value\n");
        return;
    }
    st = ensure_tree(sh);
    if (st != ATREE_OK) {
        wb_printf(w, "ERR %s: define attributes first\n", atree_strerror(st));
        return;
    }
    copy = strdup(args); /* event_fill splits its input; the original is echoed in NOTIFY */
    if (copy == NULL) {
        wb_printf(w, "ERR out of memory\n");
        return;
    }
    bad = event_fill(sh, copy);
    if (bad != NULL) {
        wb_printf(w, "ERR bad item '%s': unknown attribute or a value of the wrong type\n", bad);
        free(copy);
        return;
    }
    free(copy);
    t0 = bench_now_ns();
    st = atree_search(sh->tree, sh->ev, sh->rep);
    t1 = bench_now_ns();
    if (st != ATREE_OK) {
        wb_printf(w, "ERR %s\n", atree_strerror(st));
        return;
    }
    n = atree_report_count(sh->rep);
    wb_printf(w, "MATCH %lu", (unsigned long)n);
    if (verbose) {
        const atree_id_t *m = atree_report_matches(sh->rep);
        wb_write(w, n > 0 ? ":" : "", n > 0 ? 1 : 0);
        for (i = 0; i < n && i < MATCH_IDS_SHOWN; i++) {
            wb_printf(w, " %llu", (unsigned long long)m[i]);
        }
        if (n > MATCH_IDS_SHOWN) {
            wb_printf(w, " ... (%lu more)", (unsigned long)(n - MATCH_IDS_SHOWN));
        }
    }
    wb_write(w, "\n", 1);
    if (verbose) {
        atree_report_stats(sh->rep, &rs);
        wb_printf(w, "TIME %.1f us\n", (double)(t1 - t0) / 1000.0);
        wb_printf(w,
                  "STATS predicates_evaluated=%llu predicates_true=%llu nodes_visited=%llu "
                  "and_woken=%llu and_true=%llu or_visited=%llu\n",
                  (unsigned long long)rs.predicates_evaluated,
                  (unsigned long long)rs.predicates_matched, (unsigned long long)rs.nodes_visited,
                  (unsigned long long)rs.and_woken, (unsigned long long)rs.and_true,
                  (unsigned long long)rs.or_visited);
        wb_write(w, "OK\n", 3);
    }
    if (sh->nclients > 0) {
        notify_clients(sh, c, args);
    }
}

static void cmd_parse(struct shell *sh, char *args, struct wbuf *w)
{
    atree_expr_t *e = NULL;
    atree_error_t err;
    atree_status_t st = ensure_tree(sh);
    if (st != ATREE_OK) {
        wb_printf(w, "ERR %s: define attributes first\n", atree_strerror(st));
        return;
    }
    st = atree_expr_parse(sh->tree, args, SIZE_MAX, &e, &err);
    if (st != ATREE_OK) {
        reply_err(w, st, &err);
        return;
    }
    st = atree_expr_print(e, wb_write_cb, w);
    atree_expr_free(e);
    wb_write(w, "\n", 1);
    wb_printf(w, "%s\n", st == ATREE_OK ? "OK" : "ERR print failed");
}

static void cmd_load(struct shell *sh, struct client *c, char *args, struct wbuf *w)
{
    char *what;
    char *path = split_word(args, &what);
    FILE *f;
    char *buf;
    char *line;
    unsigned long loaded = 0;
    unsigned long failed = 0;
    unsigned long long matches = 0;
    int kind = keyword_is(what, "DEFS") ? 0
        : keyword_is(what, "EXPRS")     ? 1
        : keyword_is(what, "EVENTS")    ? 2
                                        : -1;
    if (kind < 0 || *path == '\0') {
        wb_printf(w, "ERR usage: LOAD DEFS|EXPRS|EVENTS path\n");
        return;
    }
    if (kind != 0 && ensure_tree(sh) != ATREE_OK) {
        wb_printf(w, "ERR define attributes first\n");
        return;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        wb_printf(w, "ERR cannot open %s: %s\n", path, strerror(errno));
        return;
    }
    buf = (char *)malloc(MAX_LINE);
    if (buf == NULL) {
        fclose(f);
        wb_printf(w, "ERR out of memory\n");
        return;
    }
    while ((line = read_line(f, buf)) != NULL) {
        char *tab = strchr(line, '\t');
        if (kind == 0) {
            atree_type_t ty;
            if (tab == NULL || sh->ndefs == MAX_DEFS) {
                failed++;
                continue;
            }
            *tab++ = '\0';
            while (*tab == ' ' || *tab == '\t') {
                tab++;
            }
            if (type_of(tab, &ty) != 0 || (sh->def_names[sh->ndefs] = strdup(line)) == NULL) {
                failed++;
                continue;
            }
            sh->def_types[sh->ndefs++] = ty;
            loaded++;
        } else if (kind == 1) {
            uint64_t id;
            atree_error_t err;
            if (tab == NULL) {
                failed++;
                continue;
            }
            *tab++ = '\0';
            if (parse_id(line, &id) != 0 ||
                atree_insert(sh->tree, id, tab, SIZE_MAX, &err) != ATREE_OK) {
                failed++;
                continue;
            }
            if (c != NULL) {
                (void)ids_add(c, id);
            }
            loaded++;
        } else {
            if (event_fill(sh, line) != NULL ||
                atree_search(sh->tree, sh->ev, sh->rep) != ATREE_OK) {
                failed++;
                continue;
            }
            matches += atree_report_count(sh->rep);
            loaded++;
        }
    }
    free(buf);
    fclose(f);
    if (kind == 0) {
        atree_status_t st = tree_create(sh);
        if (st != ATREE_OK) {
            wb_printf(w, "ERR %s after loading %lu attributes\n", atree_strerror(st), loaded);
            return;
        }
        wb_printf(w, "OK %lu attributes defined, %lu lines rejected; tree created\n", loaded,
                  failed);
    } else if (kind == 1) {
        wb_printf(w, "OK %lu subscribed, %lu rejected\n", loaded, failed);
    } else {
        wb_printf(w, "OK %lu events, %llu matches in total, %lu rejected\n", loaded, matches,
                  failed);
    }
}

static void cmd_stats(struct shell *sh, struct wbuf *w)
{
    atree_stats_t st;
    if (sh->tree == NULL) {
        wb_printf(w, "OK no tree yet (%lu attributes defined)\n", (unsigned long)sh->ndefs);
        return;
    }
    atree_stats(sh->tree, &st);
    wb_printf(w, "subscriptions %llu\nnodes %llu\nleaves %llu\nedges %llu\nmax_level %llu\n",
              (unsigned long long)st.subscriptions, (unsigned long long)st.nodes,
              (unsigned long long)st.leaves, (unsigned long long)st.edges,
              (unsigned long long)st.max_level);
    wb_printf(w, "indexed_leaves %llu\nscanned_leaves %llu\nreorganized %llu\nself_adjusted %llu\n",
              (unsigned long long)st.indexed_leaves, (unsigned long long)st.scanned_leaves,
              (unsigned long long)st.reorganized, (unsigned long long)st.self_adjusted);
    wb_printf(w, "strings %llu\nbytes_allocated %llu\nbytes_peak %llu\nOK\n",
              (unsigned long long)st.strings, (unsigned long long)st.bytes_allocated,
              (unsigned long long)st.bytes_peak);
}

static void cmd_validate(struct shell *sh, struct wbuf *w)
{
    char msg[256];
    atree_status_t st;
    if (sh->tree == NULL) {
        wb_printf(w, "OK no tree yet\n");
        return;
    }
    st = atree_validate(sh->tree, msg, sizeof msg);
    if (st != ATREE_OK) {
        wb_printf(w, "ERR %s: %s\n", atree_strerror(st), msg);
        return;
    }
    wb_printf(w, "OK valid\n");
}

static void cmd_dot(struct shell *sh, struct wbuf *w)
{
    if (sh->tree == NULL) {
        wb_printf(w, "ERR no tree yet\n");
        return;
    }
    if (atree_to_graphviz(sh->tree, wb_write_cb, w) != ATREE_OK) {
        wb_printf(w, "ERR graphviz export failed\n");
        return;
    }
    wb_write(w, "OK\n", 3);
}

/* Runs one command line. Returns 1 when the connection (or shell) should end. */
static int handle_line(struct shell *sh, struct client *c, char *line, struct wbuf *w)
{
    char *cmd;
    char *args;
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) {
        line[--n] = '\0';
    }
    args = split_word(line, &cmd);
    if (*cmd == '\0' || *cmd == '#') {
        return 0;
    }
    if (keyword_is(cmd, "HELP") || keyword_is(cmd, "?")) {
        cmd_help(w);
    } else if (keyword_is(cmd, "DEFINE")) {
        cmd_define(sh, args, w);
    } else if (keyword_is(cmd, "CREATE")) {
        cmd_create(sh, w);
    } else if (keyword_is(cmd, "SUBSCRIBE") || keyword_is(cmd, "SUB")) {
        cmd_subscribe(sh, c, args, w);
    } else if (keyword_is(cmd, "UNSUBSCRIBE") || keyword_is(cmd, "UNSUB")) {
        cmd_unsubscribe(sh, args, w);
    } else if (keyword_is(cmd, "EVENT")) {
        cmd_event(sh, c, args, w, 1);
    } else if (keyword_is(cmd, "PARSE")) {
        cmd_parse(sh, args, w);
    } else if (keyword_is(cmd, "LOAD")) {
        cmd_load(sh, c, args, w);
    } else if (keyword_is(cmd, "COUNT")) {
        wb_printf(w, "OK %lu\n", (unsigned long)(sh->tree != NULL ? atree_count(sh->tree) : 0));
    } else if (keyword_is(cmd, "STATS")) {
        cmd_stats(sh, w);
    } else if (keyword_is(cmd, "VALIDATE")) {
        cmd_validate(sh, w);
    } else if (keyword_is(cmd, "DOT")) {
        cmd_dot(sh, w);
    } else if (keyword_is(cmd, "QUIT") || keyword_is(cmd, "EXIT")) {
        wb_write(w, "OK bye\n", 7);
        return 1;
    } else {
        wb_printf(w, "ERR unknown command '%s' (HELP lists them)\n", cmd);
    }
    return 0;
}

/* ---- stdin mode ----------------------------------------------------------- */

static int run_stdin(struct shell *sh)
{
    char *buf = (char *)malloc(MAX_LINE);
    struct wbuf w;
    w.fd = 1;
    w.n = 0;
    if (buf == NULL) {
        return 2;
    }
    while (!sh->quit && fgets(buf, MAX_LINE, stdin) != NULL) {
        if (handle_line(sh, NULL, buf, &w)) {
            sh->quit = 1;
        }
        wb_flush(&w);
    }
    free(buf);
    return 0;
}

/* ---- sockets -------------------------------------------------------------- */

static int is_number(const char *s)
{
    if (*s == '\0') {
        return 0;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
    }
    return 1;
}

/* Listens on 127.0.0.1:PORT (spec all digits) or on a Unix socket at PATH. */
static int open_listener(const char *spec)
{
    int fd;
    int one = 1;
    if (is_number(spec)) {
        struct sockaddr_in sa;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)strtoul(spec, NULL, 10));
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
            close(fd);
            return -1;
        }
    } else {
        struct sockaddr_un sa;
        if (strlen(spec) >= sizeof sa.sun_path) {
            errno = ENAMETOOLONG;
            return -1;
        }
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, spec);
        (void)unlink(spec);
        if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
            close(fd);
            return -1;
        }
    }
    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int open_connection(const char *host, const char *port)
{
    int fd;
    if (port == NULL) {
        struct sockaddr_un sa;
        if (strlen(host) >= sizeof sa.sun_path) {
            errno = ENAMETOOLONG;
            return -1;
        }
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, host);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    } else {
        struct addrinfo hints;
        struct addrinfo *res = NULL;
        struct addrinfo *ai;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, port, &hints, &res) != 0) {
            return -1;
        }
        fd = -1;
        for (ai = res; ai != NULL; ai = ai->ai_next) {
            fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) {
                continue;
            }
            if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
                break;
            }
            close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
        return fd;
    }
}

static void client_close(struct shell *sh, size_t k)
{
    struct client *c = &sh->clients[k];
    size_t i;
    /* a connection's continuous queries end with it */
    for (i = 0; i < c->nids && sh->tree != NULL; i++) {
        (void)atree_delete(sh->tree, c->ids[i]);
    }
    close(c->fd);
    free(c->buf);
    free(c->ids);
    sh->clients[k] = sh->clients[--sh->nclients];
}

/* Consumes complete lines from a client's buffer. Returns 1 to close it. */
static int client_drain(struct shell *sh, struct client *c)
{
    struct wbuf w;
    size_t start = 0;
    size_t i;
    int done = 0;
    w.fd = c->fd;
    w.n = 0;
    for (i = 0; i < c->len && !done; i++) {
        if (c->buf[i] == '\n') {
            c->buf[i] = '\0';
            done = handle_line(sh, c, c->buf + start, &w);
            start = i + 1;
        }
    }
    wb_flush(&w);
    if (done) {
        return 1;
    }
    memmove(c->buf, c->buf + start, c->len - start);
    c->len -= start;
    if (c->len == MAX_LINE) {
        w.n = 0;
        wb_printf(&w, "ERR line too long\n");
        wb_flush(&w);
        return 1;
    }
    return 0;
}

static int run_server(struct shell *sh, const char *spec)
{
    int lfd = open_listener(spec);
    if (lfd < 0) {
        fprintf(stderr, "atree_shell: cannot listen on %s: %s\n", spec, strerror(errno));
        return 1;
    }
    printf("LISTENING %s\n", spec);
    fflush(stdout);
    for (;;) {
        fd_set rd;
        int maxfd = lfd;
        size_t k;
        FD_ZERO(&rd);
        FD_SET(lfd, &rd);
        for (k = 0; k < sh->nclients; k++) {
            FD_SET(sh->clients[k].fd, &rd);
            if (sh->clients[k].fd > maxfd) {
                maxfd = sh->clients[k].fd;
            }
        }
        if (select(maxfd + 1, &rd, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (FD_ISSET(lfd, &rd)) {
            int fd = accept(lfd, NULL, NULL);
            if (fd >= 0) {
                if (sh->nclients == MAX_CLIENTS) {
                    close(fd);
                } else {
                    struct client *c = &sh->clients[sh->nclients];
                    memset(c, 0, sizeof *c);
                    c->fd = fd;
                    c->buf = (char *)malloc(MAX_LINE);
                    if (c->buf == NULL) {
                        close(fd);
                    } else {
                        sh->nclients++;
                    }
                }
            }
        }
        for (k = 0; k < sh->nclients;) {
            struct client *c = &sh->clients[k];
            if (FD_ISSET(c->fd, &rd)) {
                ssize_t n = read(c->fd, c->buf + c->len, MAX_LINE - c->len);
                if (n <= 0 || (c->len += (size_t)n, client_drain(sh, c))) {
                    client_close(sh, k);
                    continue;
                }
            }
            k++;
        }
    }
    close(lfd);
    return 0;
}

static int run_client(const char *host, const char *port)
{
    int fd = open_connection(host, port);
    char *buf;
    int stdin_open = 1;
    if (fd < 0) {
        fprintf(stderr, "atree_shell: cannot connect to %s%s%s: %s\n", host, port ? ":" : "",
                port ? port : "", strerror(errno));
        return 1;
    }
    buf = (char *)malloc(MAX_LINE);
    if (buf == NULL) {
        close(fd);
        return 2;
    }
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        if (stdin_open) {
            FD_SET(0, &rd);
        }
        if (select(fd + 1, &rd, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (FD_ISSET(fd, &rd)) {
            ssize_t n = read(fd, buf, MAX_LINE);
            if (n <= 0) {
                break;
            }
            (void)fwrite(buf, 1, (size_t)n, stdout);
            fflush(stdout);
        }
        if (stdin_open && FD_ISSET(0, &rd)) {
            ssize_t n = read(0, buf, MAX_LINE);
            if (n <= 0) {
                stdin_open = 0;
                (void)write(fd, "QUIT\n", 5);
            } else {
                (void)write(fd, buf, (size_t)n);
            }
        }
    }
    free(buf);
    close(fd);
    return 0;
}

/* ---- main ----------------------------------------------------------------- */

static void shell_free(struct shell *sh)
{
    size_t i;
    while (sh->nclients > 0) {
        client_close(sh, sh->nclients - 1);
    }
    tree_drop(sh);
    for (i = 0; i < sh->ndefs; i++) {
        free(sh->def_names[i]);
    }
}

static int load_file(struct shell *sh, const char *what, const char *path)
{
    char line[4096];
    struct wbuf w;
    w.fd = 1;
    w.n = 0;
    snprintf(line, sizeof line, "LOAD %s %s", what, path);
    handle_line(sh, NULL, line, &w);
    wb_flush(&w);
    return 0;
}

int main(int argc, char **argv)
{
    static struct shell sh;
    const char *listen_spec = NULL;
    const char *connect_host = NULL;
    const char *connect_port = NULL;
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen_spec = argv[++i];
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            connect_host = argv[++i];
            if (i + 1 < argc && is_number(argv[i + 1])) {
                connect_port = argv[++i];
            }
        } else if (strcmp(argv[i], "--defs") == 0 && i + 1 < argc) {
            load_file(&sh, "DEFS", argv[++i]);
        } else if (strcmp(argv[i], "--exprs") == 0 && i + 1 < argc) {
            load_file(&sh, "EXPRS", argv[++i]);
        } else if (strcmp(argv[i], "--events") == 0 && i + 1 < argc) {
            load_file(&sh, "EVENTS", argv[++i]);
        } else {
            fprintf(stderr,
                    "usage: atree_shell [--defs F] [--exprs F] [--events F]\n"
                    "       atree_shell --listen PORT|PATH [--defs F ...]\n"
                    "       atree_shell --connect HOST PORT | --connect PATH\n");
            return 2;
        }
    }
    if (connect_host != NULL) {
        return run_client(connect_host, connect_port);
    }
    rc = listen_spec != NULL ? run_server(&sh, listen_spec) : run_stdin(&sh);
    shell_free(&sh);
    return rc;
}
