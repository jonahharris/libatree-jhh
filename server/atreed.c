/*
 * atreed: a continuous-query server over libatree.
 *
 * One A-Tree, served to Redis, Postgres and HTTP clients at once on one
 * port, on pogocache's networking core (deps/pogocache, MIT): the first
 * bytes of a connection pick the protocol, every protocol's request is
 * parsed into one argument list, and the commands below are written once.
 *
 *   redis-cli -p 7777 ATREE.SUBSCRIBE 7 "price > 10 and country = 'US'"
 *   redis-cli -p 7777 SUBSCRIBE atree:7            # pushes each matching event
 *   redis-cli -p 7777 ATREE.EVENT '{"price": 12, "country": "US"}'
 *
 *   psql -h 127.0.0.1 -p 7777 -c "ATREE.SUBSCRIBE 7 'price > 10'"
 *   psql ... -c "LISTEN atree_7"                    # notification per match
 *   WATCH 7 streams one row per match to a client that reads rows as they
 *   arrive (libpq single-row mode, pgx); psql itself shows nothing until
 *   the connection ends.
 *
 *   curl -XPUT localhost:7777/queries/7 -d "price > 10"
 *   curl -N localhost:7777/subscribe/7              # Server-Sent Events
 *   curl localhost:7777/events -d '{"price": 12}'
 *
 * Commands (RESP and Postgres; case-insensitive; the ATREE. prefix is
 * optional except for SUBSCRIBE/UNSUBSCRIBE, whose bare forms are Redis
 * pub/sub):
 *   ATREE.DEFINE name type      ATREE.CREATE          ATREE.SUBSCRIBE id expr
 *   ATREE.UNSUBSCRIBE id        ATREE.EVENT payload   ATREE.COUNT
 *   ATREE.STATS                 ATREE.VALIDATE        HELP  PING  QUIT
 *   SUBSCRIBE atree:<id>|atree:*   UNSUBSCRIBE ...     (Redis pub/sub delivery)
 *   WATCH <id>|*   LISTEN atree_<id>|atree_all         (Postgres delivery)
 *
 * The event payload is either the bench_file line format, a=v;b=v, or a
 * JSON object. A matched event is delivered to every subscriber of its
 * query id (and of the catch-all) in that subscriber's protocol.
 *
 * The server runs one event-loop thread, so the tree needs no lock and a
 * push to another connection is a plain write to its buffer followed by a
 * flush. The library itself is untouched by any of this.
 *
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atree.h"

#include "../bench/bench_format.h"
#include "args.h"
#include "cmds.h"
#include "conn.h"
#include "json.h"
#include "net.h"
#include "util.h"
#include "xmalloc.h"

/* ---- globals the vendored pogocache units expect --------------------------- */

const int verb = 0;
const char *version = "0.1.0";
const char *auth = NULL;
const bool useauth = false;
const int useallocator = ALLOCATOR_STOCK;
const bool usetrackallocs = false;
const bool usetls = false;
const char *tlscertfile = NULL;
const char *tlscacertfile = NULL;
const char *tlskeyfile = NULL;

/* ---- state -------------------------------------------------------------------- */

#define MAX_DEFS 4096
#define SUB_ALL UINT64_MAX

#if defined(__GNUC__)
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#else
#define PRINTF_LIKE(f, a)
#endif

enum sub_kind { SUB_RESP, SUB_SSE, SUB_PG_WATCH, SUB_PG_LISTEN };

struct sub {
    struct conn *conn;
    enum sub_kind kind;
    uint64_t id; /* query id, or SUB_ALL */
};

static struct {
    atree_t *tree;
    atree_event_t *ev;
    atree_report_t *rep;
    char *def_names[MAX_DEFS];
    atree_type_t def_types[MAX_DEFS];
    size_t ndefs;
    struct sub *subs;
    size_t nsubs;
    size_t capsubs;
    char *scratch; /* event payload copy / JSON unescape buffer */
    size_t scratch_cap;
    uint64_t events;
    uint64_t deliveries;
} S;

static const char *HELP =
    "ATREE.DEFINE name type        bool int float string int_list string_list\n"
    "ATREE.CREATE                  (re)build the tree over the defined attributes\n"
    "ATREE.SUBSCRIBE id expr       register a continuous query\n"
    "ATREE.UNSUBSCRIBE id\n"
    "ATREE.EVENT payload           a=v;b=v or a JSON object; replies with the matched ids\n"
    "ATREE.COUNT  ATREE.STATS  ATREE.VALIDATE  HELP  PING  QUIT\n"
    "SUBSCRIBE atree:<id> | atree:*          Redis pub/sub delivery of matching events\n"
    "WATCH <id> | *                          Postgres: one streamed row per match\n"
    "LISTEN atree_<id> | atree_all           Postgres: async notifications\n"
    "HTTP: PUT /queries/<id> (body: expr)  DELETE /queries/<id>  POST /events (body)\n"
    "      POST /attributes (lines: name type)  POST /create  GET /subscribe/<id>|all (SSE)\n"
    "      GET /stats  GET /validate  GET /count  GET /help\n";

/* ---- replies, by protocol -------------------------------------------------------- */

static void reply_ok(struct conn *conn, const char *msg)
{
    if (conn_proto(conn) == PROTO_POSTGRES) {
        pg_write_simple_row_str_ready(conn, "result", msg, "OK");
    } else {
        conn_write_string(conn, msg);
    }
}

static void reply_err(struct conn *conn, const char *fmt, ...) PRINTF_LIKE(2, 3);

static void reply_err(struct conn *conn, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    conn_write_error(conn, msg);
}

static void reply_int(struct conn *conn, int64_t v)
{
    if (conn_proto(conn) == PROTO_POSTGRES) {
        pg_write_simple_row_i64_ready(conn, "result", v, "OK");
    } else {
        conn_write_int(conn, v);
    }
}

static void reply_text(struct conn *conn, const char *text)
{
    if (conn_proto(conn) == PROTO_POSTGRES) {
        pg_write_simple_row_str_ready(conn, "result", text, "OK");
    } else {
        conn_write_bulk(conn, text, strlen(text));
    }
}

/* ---- tree lifecycle --------------------------------------------------------------- */

static void tree_drop(void)
{
    atree_report_destroy(S.rep);
    atree_event_destroy(S.ev);
    atree_destroy(S.tree);
    S.rep = NULL;
    S.ev = NULL;
    S.tree = NULL;
}

static atree_status_t tree_create(void)
{
    atree_attr_def_t defs[MAX_DEFS];
    size_t i;
    atree_status_t st;
    if (S.ndefs == 0) {
        return ATREE_ERR_INVALID_ARG;
    }
    for (i = 0; i < S.ndefs; i++) {
        defs[i].name = S.def_names[i];
        defs[i].type = S.def_types[i];
    }
    tree_drop();
    st = atree_create(NULL, defs, S.ndefs, &S.tree);
    if (st == ATREE_OK) {
        st = atree_event_create(S.tree, &S.ev);
    }
    if (st == ATREE_OK) {
        st = atree_report_create(S.tree, &S.rep);
    }
    if (st != ATREE_OK) {
        tree_drop();
    }
    return st;
}

static atree_status_t ensure_tree(void)
{
    return S.tree != NULL ? ATREE_OK : tree_create();
}

static int define_attr(const char *name, size_t namelen, const char *type, size_t typelen)
{
    char tbuf[32];
    atree_type_t ty;
    if (namelen == 0 || typelen == 0 || typelen >= sizeof tbuf || S.ndefs == MAX_DEFS) {
        return 1;
    }
    memcpy(tbuf, type, typelen);
    tbuf[typelen] = '\0';
    if (type_of(tbuf, &ty) != 0) {
        return 1;
    }
    S.def_names[S.ndefs] = xmalloc(namelen + 1);
    memcpy(S.def_names[S.ndefs], name, namelen);
    S.def_names[S.ndefs][namelen] = '\0';
    S.def_types[S.ndefs] = ty;
    S.ndefs++;
    return 0;
}

/* ---- subscribers ------------------------------------------------------------------ */

static void sub_add(struct conn *conn, enum sub_kind kind, uint64_t id)
{
    size_t i;
    for (i = 0; i < S.nsubs; i++) {
        if (S.subs[i].conn == conn && S.subs[i].kind == kind && S.subs[i].id == id) {
            return;
        }
    }
    if (S.nsubs == S.capsubs) {
        S.capsubs = S.capsubs == 0 ? 16 : S.capsubs * 2;
        S.subs = xrealloc(S.subs, S.capsubs * sizeof *S.subs);
    }
    S.subs[S.nsubs].conn = conn;
    S.subs[S.nsubs].kind = kind;
    S.subs[S.nsubs].id = id;
    S.nsubs++;
}

/* Removes the subscriptions of conn (all of them when id is SUB_ALL and
 * kind is -1). Returns how many were removed. */
static size_t sub_remove(struct conn *conn, int kind, uint64_t id, bool all)
{
    size_t i = 0;
    size_t removed = 0;
    while (i < S.nsubs) {
        struct sub *s = &S.subs[i];
        if (s->conn == conn && (all || ((int)s->kind == kind && s->id == id))) {
            *s = S.subs[--S.nsubs];
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

static size_t sub_count(struct conn *conn, enum sub_kind kind)
{
    size_t i;
    size_t n = 0;
    for (i = 0; i < S.nsubs; i++) {
        n += S.subs[i].conn == conn && S.subs[i].kind == kind;
    }
    return n;
}

void evclosed_hook(struct conn *conn)
{
    sub_remove(conn, 0, 0, true);
}

/* ---- payload parsing ------------------------------------------------------------- */

static void scratch_reserve(size_t n)
{
    if (S.scratch_cap < n) {
        S.scratch_cap = n < 4096 ? 4096 : n * 2;
        S.scratch = xrealloc(S.scratch, S.scratch_cap);
    }
}

struct json_ctx {
    const char *bad; /* member name that failed, or NULL */
    size_t badlen;
    char why[128];
};

static int json_member(void *ctxp, const char *key, size_t keylen, const struct json_value *v)
{
    struct json_ctx *ctx = ctxp;
    char name[128];
    atree_attr_id_t id;
    atree_type_t ty;
    atree_status_t st = ATREE_ERR_TYPE_MISMATCH;
    if (keylen >= sizeof name) {
        snprintf(ctx->why, sizeof ctx->why, "attribute name too long");
        goto bad;
    }
    memcpy(name, key, keylen);
    name[keylen] = '\0';
    id = atree_attr_lookup(S.tree, name);
    if (id == ATREE_ATTR_INVALID) {
        snprintf(ctx->why, sizeof ctx->why, "unknown attribute");
        goto bad;
    }
    if (v->kind == JSON_NULL) {
        st = atree_event_set_undefined_id(S.ev, id);
        goto done;
    }
    ty = atree_attr_type(S.tree, id);
    switch (ty) {
    case ATREE_TYPE_BOOL:
        if (v->kind == JSON_BOOL) {
            st = atree_event_set_bool_id(S.ev, id, v->b);
        }
        break;
    case ATREE_TYPE_INT:
        if (v->kind == JSON_INT) {
            st = atree_event_set_int_id(S.ev, id, v->i);
        }
        break;
    case ATREE_TYPE_FLOAT:
        if (v->kind == JSON_FLOAT) {
            st = atree_event_set_float_id(S.ev, id, v->f);
        } else if (v->kind == JSON_INT) {
            st = atree_event_set_float_id(S.ev, id, (double)v->i);
        }
        break;
    case ATREE_TYPE_STRING:
        if (v->kind == JSON_STRING) {
            st = atree_event_set_string_id(S.ev, id, v->s, v->slen);
        }
        break;
    case ATREE_TYPE_INT_LIST:
        if (v->kind == JSON_ARRAY && (v->nitems == 0 || !v->items_are_strings)) {
            st = atree_event_set_int_list_id(S.ev, id, v->ints, v->nitems);
        }
        break;
    case ATREE_TYPE_STRING_LIST:
        if (v->kind == JSON_ARRAY && (v->nitems == 0 || v->items_are_strings)) {
            st = atree_event_set_string_list_id(S.ev, id, v->strs, v->slens, v->nitems);
        }
        break;
    default:
        break;
    }
done:
    if (st == ATREE_OK) {
        return 0;
    }
    snprintf(ctx->why, sizeof ctx->why, "%s", atree_strerror(st));
bad:
    ctx->bad = key;
    ctx->badlen = keylen;
    return 1;
}

/* Fills S.ev from the payload. Returns 0, or 1 with *err describing why. */
static int event_fill(const char *payload, size_t len, char *err, size_t errcap)
{
    const char *p = payload;
    const char *e = payload + len;
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    atree_event_clear(S.ev);
    if (p < e && *p == '{') {
        struct json_ctx ctx;
        size_t errpos = 0;
        int rc;
        memset(&ctx, 0, sizeof ctx);
        scratch_reserve(len + 1);
        rc = json_parse_object(p, (size_t)(e - p), S.scratch, json_member, &ctx, &errpos);
        if (rc < 0) {
            snprintf(err, errcap, "invalid JSON at byte %zu", errpos);
            return 1;
        }
        if (rc > 0) {
            snprintf(err, errcap, "\"%.*s\": %s", (int)ctx.badlen, ctx.bad, ctx.why);
            return 1;
        }
        return 0;
    }
    /* a=v;b=v: set_item needs writable, NUL-terminated items */
    scratch_reserve(len + 1);
    memcpy(S.scratch, payload, len);
    S.scratch[len] = '\0';
    {
        char *item = S.scratch;
        while (item != NULL && *item != '\0') {
            char *semi = strchr(item, ';');
            if (semi != NULL) {
                *semi = '\0';
            }
            while (*item == ' ') {
                item++;
            }
            if (*item != '\0' && set_item(S.tree, S.ev, item) != 0) {
                snprintf(err, errcap, "bad item '%s': unknown attribute or wrong type", item);
                return 1;
            }
            item = semi != NULL ? semi + 1 : NULL;
        }
    }
    return 0;
}

/* ---- delivery --------------------------------------------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* Appends payload as a JSON value: raw when it is JSON, a string otherwise. */
static void append_payload_json(struct buf *b, const char *payload, size_t len)
{
    size_t i;
    const char *p = payload;
    while (len > 0 && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
        len--;
    }
    if (len > 0 && *p == '{') {
        buf_append(b, p, len);
        return;
    }
    buf_append_byte(b, '"');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '"' || c == '\\') {
            buf_append_byte(b, '\\');
            buf_append_byte(b, (char)c);
        } else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            buf_append(b, esc, 6);
        } else {
            buf_append_byte(b, (char)c);
        }
    }
    buf_append_byte(b, '"');
}

/* The catch-all channels carry no id in their name, so their payload is the
 * SSE object, {"id":N,"event":...}; a per-id channel delivers the event text. */
static void catch_all_payload(struct buf *b, uint64_t id, const char *payload, size_t len)
{
    char head[24];
    buf_append(b, "{\"id\":", 6);
    buf_append(b, head, (size_t)snprintf(head, sizeof head, "%" PRIu64, id));
    buf_append(b, ",\"event\":", 9);
    append_payload_json(b, payload, len);
    buf_append_byte(b, '}');
}

static void push_resp(struct conn *conn, uint64_t sub_id, uint64_t id, const char *payload,
                      size_t len)
{
    char chan[48];
    size_t n = sub_id == SUB_ALL ? (size_t)snprintf(chan, sizeof chan, "atree:*")
                                 : (size_t)snprintf(chan, sizeof chan, "atree:%" PRIu64, sub_id);
    conn_write_array(conn, 3);
    conn_write_bulk(conn, "message", 7);
    conn_write_bulk(conn, chan, n);
    if (sub_id == SUB_ALL) {
        struct buf b = {0};
        catch_all_payload(&b, id, payload, len);
        conn_write_bulk(conn, b.data, b.len);
        buf_clear(&b);
    } else {
        conn_write_bulk(conn, payload, len);
    }
}

static void push_sse(struct conn *conn, uint64_t id, const char *payload, size_t len)
{
    struct buf b = {0};
    char head[64];
    buf_append(&b, "event: match\ndata: {\"id\":", 25);
    buf_append(&b, head, (size_t)snprintf(head, sizeof head, "%" PRIu64, id));
    buf_append(&b, ",\"event\":", 9);
    append_payload_json(&b, payload, len);
    buf_append(&b, "}\n\n", 3);
    conn_write_raw(conn, b.data, b.len);
    buf_clear(&b);
}

static void push_pg_row(struct conn *conn, uint64_t id, const char *payload, size_t len)
{
    char idtext[24];
    const char *cols[2];
    size_t lens[2];
    cols[0] = idtext;
    lens[0] = (size_t)snprintf(idtext, sizeof idtext, "%" PRIu64, id);
    cols[1] = payload;
    lens[1] = len;
    pg_write_row_data(conn, cols, lens, 2);
}

/* Postgres NotificationResponse: 'A', int32 length, int32 pid, channel, payload. */
static void push_pg_notify(struct conn *conn, uint64_t sub_id, uint64_t id, const char *payload,
                           size_t len)
{
    char chan[48];
    size_t chanlen = sub_id == SUB_ALL
        ? (size_t)snprintf(chan, sizeof chan, "atree_all")
        : (size_t)snprintf(chan, sizeof chan, "atree_%" PRIu64, sub_id);
    struct buf b = {0};
    size_t size;
    char *msg;
    char *p;
    if (sub_id == SUB_ALL) {
        catch_all_payload(&b, id, payload, len);
        payload = b.data;
        len = b.len;
    }
    size = 4 + 4 + chanlen + 1 + len + 1;
    msg = xmalloc(1 + size);
    p = msg;
    *p++ = 'A';
    p[0] = (char)((size >> 24) & 0xff);
    p[1] = (char)((size >> 16) & 0xff);
    p[2] = (char)((size >> 8) & 0xff);
    p[3] = (char)(size & 0xff);
    p += 4;
    memset(p, 0, 4); /* pid */
    p += 4;
    memcpy(p, chan, chanlen + 1);
    p += chanlen + 1;
    memcpy(p, payload, len);
    p[len] = '\0';
    conn_write_raw(conn, msg, 1 + size);
    xfree(msg);
    buf_clear(&b);
}

/* Delivers the current report's matches to every subscriber: a per-id
 * subscriber once, a catch-all subscriber once per matched id. */
static void deliver(const char *payload, size_t len)
{
    const atree_id_t *m = atree_report_matches(S.rep);
    size_t nm = atree_report_count(S.rep);
    size_t i;
    size_t k;
    if (nm == 0 || S.nsubs == 0) {
        return;
    }
    for (k = 0; k < S.nsubs; k++) {
        struct sub *s = &S.subs[k];
        bool wrote = false;
        for (i = 0; i < nm; i++) {
            uint64_t id = m[i];
            if (s->id != SUB_ALL) {
                /* matches are sorted: binary-search for this subscriber's id */
                size_t lo = 0;
                size_t hi = nm;
                while (lo < hi) {
                    size_t mid = lo + (hi - lo) / 2;
                    if (m[mid] < s->id) {
                        lo = mid + 1;
                    } else {
                        hi = mid;
                    }
                }
                if (lo == nm || m[lo] != s->id) {
                    break;
                }
                id = s->id;
                i = nm; /* one delivery for this subscriber */
            }
            switch (s->kind) {
            case SUB_RESP:
                push_resp(s->conn, s->id, id, payload, len);
                break;
            case SUB_SSE:
                push_sse(s->conn, id, payload, len);
                break;
            case SUB_PG_WATCH:
                push_pg_row(s->conn, id, payload, len);
                break;
            case SUB_PG_LISTEN:
            default:
                push_pg_notify(s->conn, s->id, id, payload, len);
                break;
            }
            wrote = true;
            S.deliveries++;
        }
        if (wrote) {
            conn_flush(s->conn);
        }
    }
}

/* ---- commands --------------------------------------------------------------------- */

static bool arg_is(struct args *args, int i, const char *name)
{
    size_t len;
    const char *a = args_at(args, i, &len);
    size_t j;
    for (j = 0; j < len && name[j] != '\0'; j++) {
        char c = a[j];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != name[j]) {
            return false;
        }
    }
    return j == len && name[j] == '\0';
}

/* Joins args[from..] with single spaces into the scratch buffer (a telnet
 * client types an expression as many words). */
static const char *join_args(struct args *args, int from, size_t *outlen)
{
    size_t total = 0;
    int i;
    char *p;
    for (i = from; i < args_count(args); i++) {
        size_t len;
        args_at(args, i, &len);
        total += len + 1;
    }
    scratch_reserve(total + 1);
    p = S.scratch;
    for (i = from; i < args_count(args); i++) {
        size_t len;
        const char *a = args_at(args, i, &len);
        if (i > from) {
            *p++ = ' ';
        }
        memcpy(p, a, len);
        p += len;
    }
    *p = '\0';
    *outlen = (size_t)(p - S.scratch);
    return S.scratch;
}

static bool arg_id(struct args *args, int i, uint64_t *id)
{
    size_t len;
    const char *a = args_at(args, i, &len);
    if (len == 1 && a[0] == '*') {
        *id = SUB_ALL;
        return true;
    }
    return parse_u64(a, len, id);
}

static void cmd_define(struct conn *conn, struct args *args)
{
    size_t nlen;
    size_t tlen;
    const char *name;
    const char *type;
    if (args_count(args) != 3) {
        reply_err(conn, "ERR usage: ATREE.DEFINE name bool|int|float|string|int_list|string_list");
        return;
    }
    name = args_at(args, 1, &nlen);
    type = args_at(args, 2, &tlen);
    if (define_attr(name, nlen, type, tlen) != 0) {
        reply_err(conn, "ERR bad attribute definition");
        return;
    }
    reply_ok(conn, S.tree != NULL ? "OK (ATREE.CREATE to apply; queries will be dropped)" : "OK");
}

static void cmd_create(struct conn *conn)
{
    atree_status_t st = tree_create();
    if (st != ATREE_OK) {
        reply_err(conn, "ERR %s%s", atree_strerror(st),
                  S.ndefs == 0 ? ": define attributes first" : "");
        return;
    }
    sub_remove(NULL, 0, 0, false); /* no-op; subscribers keep their channels */
    reply_ok(conn, "OK");
}

static void cmd_subscribe_query(struct conn *conn, struct args *args)
{
    uint64_t id;
    size_t len;
    const char *expr;
    atree_error_t err;
    atree_status_t st;
    const char *idtext = args_count(args) < 3 ? NULL : args_at(args, 1, &len);
    /* Two statements: an out-parameter read in the same argument list that
     * sets it has unspecified order (x86-64 gcc evaluates right to left). */
    if (idtext == NULL || !parse_u64(idtext, len, &id)) {
        reply_err(conn, "ERR usage: ATREE.SUBSCRIBE id expression");
        return;
    }
    if (ensure_tree() != ATREE_OK) {
        reply_err(conn, "ERR define attributes first");
        return;
    }
    expr = join_args(args, 2, &len);
    st = atree_insert(S.tree, id, expr, len, &err);
    if (st != ATREE_OK) {
        if (err.offset != SIZE_MAX) {
            reply_err(conn, "ERR %s: %s (at offset %zu)", atree_strerror(st), err.message,
                      err.offset);
        } else {
            reply_err(conn, "ERR %s: %s", atree_strerror(st), err.message);
        }
        return;
    }
    reply_ok(conn, "OK");
}

static void cmd_unsubscribe_query(struct conn *conn, struct args *args)
{
    uint64_t id;
    size_t len;
    atree_status_t st;
    const char *idtext = args_count(args) != 2 ? NULL : args_at(args, 1, &len);
    if (idtext == NULL || !parse_u64(idtext, len, &id)) {
        reply_err(conn, "ERR usage: ATREE.UNSUBSCRIBE id");
        return;
    }
    st = S.tree == NULL ? ATREE_ERR_NOT_FOUND : atree_delete(S.tree, id);
    if (st != ATREE_OK) {
        reply_err(conn, "ERR %s", atree_strerror(st));
        return;
    }
    reply_ok(conn, "OK");
}

/* Runs the search for a payload; on success the report holds the matches. */
static atree_status_t ingest(const char *payload, size_t len, char *err, size_t errcap,
                             uint64_t *elapsed_ns)
{
    atree_status_t st = ensure_tree();
    uint64_t t0;
    if (st != ATREE_OK) {
        snprintf(err, errcap, "define attributes first");
        return st;
    }
    if (event_fill(payload, len, err, errcap) != 0) {
        return ATREE_ERR_INVALID_ARG;
    }
    t0 = now_ns();
    st = atree_search(S.tree, S.ev, S.rep);
    *elapsed_ns = now_ns() - t0;
    if (st != ATREE_OK) {
        snprintf(err, errcap, "%s", atree_strerror(st));
        return st;
    }
    S.events++;
    deliver(payload, len);
    return ATREE_OK;
}

static void cmd_event(struct conn *conn, struct args *args)
{
    size_t len;
    const char *payload;
    char err[256];
    uint64_t ns = 0;
    atree_status_t st;
    size_t n;
    size_t i;
    if (args_count(args) < 2) {
        reply_err(conn, "ERR usage: ATREE.EVENT payload (a=v;b=v or a JSON object)");
        return;
    }
    if (args_count(args) == 2) {
        payload = args_at(args, 1, &len);
    } else {
        /* k v k v ... from a Redis client: fold into the line format */
        struct buf b = {0};
        char *copy;
        for (i = 1; i + 1 < (size_t)args_count(args); i += 2) {
            size_t kl;
            size_t vl;
            const char *k = args_at(args, (int)i, &kl);
            const char *v = args_at(args, (int)i + 1, &vl);
            if (i > 1) {
                buf_append_byte(&b, ';');
            }
            buf_append(&b, k, kl);
            buf_append_byte(&b, '=');
            buf_append(&b, v, vl);
        }
        len = b.len;
        copy = xmalloc(len + 1); /* event_fill uses the shared scratch itself */
        memcpy(copy, b.data, len);
        copy[len] = '\0';
        buf_clear(&b);
        st = ingest(copy, len, err, sizeof err, &ns);
        xfree(copy);
        goto reply;
    }
    st = ingest(payload, len, err, sizeof err, &ns);
reply:
    if (st != ATREE_OK) {
        reply_err(conn, "ERR %s", err);
        return;
    }
    n = atree_report_count(S.rep);
    if (conn_proto(conn) == PROTO_POSTGRES) {
        const char *fields[1] = {"match"};
        const atree_id_t *m = atree_report_matches(S.rep);
        pg_write_row_desc(conn, fields, 1);
        for (i = 0; i < n; i++) {
            char idtext[24];
            const char *cols[1] = {idtext};
            size_t lens[1];
            lens[0] = (size_t)snprintf(idtext, sizeof idtext, "%" PRIu64, m[i]);
            pg_write_row_data(conn, cols, lens, 1);
        }
        pg_write_completef(conn, "EVENT %zu", n);
        pg_write_ready(conn, 'I');
    } else {
        const atree_id_t *m = atree_report_matches(S.rep);
        conn_write_array(conn, n);
        for (i = 0; i < n; i++) {
            conn_write_int(conn, (int64_t)m[i]);
        }
    }
}

/* Redis INFO style, with the line ending of the protocol (CRLF for RESP). */
static void stats_text(struct buf *b, const char *eol)
{
    atree_stats_t st;
    char line[128];
    if (S.tree == NULL) {
        buf_append(b, line, (size_t)snprintf(line, sizeof line, "tree:none%s", eol));
        return;
    }
    atree_stats(S.tree, &st);
#define LINE(k, v)                                                                                 \
    buf_append(b, line,                                                                            \
               (size_t)snprintf(line, sizeof line, "%s:%" PRIu64 "%s", k, (uint64_t)(v), eol))
    LINE("subscriptions", st.subscriptions);
    LINE("nodes", st.nodes);
    LINE("leaves", st.leaves);
    LINE("edges", st.edges);
    LINE("max_level", st.max_level);
    LINE("indexed_leaves", st.indexed_leaves);
    LINE("scanned_leaves", st.scanned_leaves);
    LINE("reorganized", st.reorganized);
    LINE("self_adjusted", st.self_adjusted);
    LINE("strings", st.strings);
    LINE("bytes_allocated", st.bytes_allocated);
    LINE("bytes_peak", st.bytes_peak);
    LINE("events", S.events);
    LINE("deliveries", S.deliveries);
    LINE("subscribers", S.nsubs);
#undef LINE
}

static void stats_json(struct buf *b)
{
    atree_stats_t st;
    char line[160];
    memset(&st, 0, sizeof st);
    if (S.tree != NULL) {
        atree_stats(S.tree, &st);
    }
    buf_append(b, line,
               (size_t)snprintf(line, sizeof line,
                                "{\"subscriptions\":%" PRIu64 ",\"nodes\":%" PRIu64
                                ",\"leaves\":%" PRIu64 ",\"edges\":%" PRIu64 ",",
                                st.subscriptions, st.nodes, st.leaves, st.edges));
    buf_append(b, line,
               (size_t)snprintf(line, sizeof line,
                                "\"max_level\":%" PRIu64 ",\"indexed_leaves\":%" PRIu64
                                ",\"scanned_leaves\":%" PRIu64 ",",
                                st.max_level, st.indexed_leaves, st.scanned_leaves));
    buf_append(b, line,
               (size_t)snprintf(line, sizeof line,
                                "\"reorganized\":%" PRIu64 ",\"self_adjusted\":%" PRIu64
                                ",\"strings\":%" PRIu64 ",",
                                st.reorganized, st.self_adjusted, st.strings));
    buf_append(
        b, line,
        (size_t)snprintf(line, sizeof line,
                         "\"bytes_allocated\":%" PRIu64 ",\"bytes_peak\":%" PRIu64
                         ",\"events\":%" PRIu64 ",\"deliveries\":%" PRIu64 ",\"subscribers\":%zu}",
                         st.bytes_allocated, st.bytes_peak, S.events, S.deliveries, S.nsubs));
}

static void cmd_stats(struct conn *conn)
{
    struct buf b = {0};
    stats_text(&b, conn_proto(conn) == PROTO_RESP ? "\r\n" : "\n");
    buf_append_byte(&b, '\0');
    reply_text(conn, b.data);
    buf_clear(&b);
}

static void cmd_validate(struct conn *conn)
{
    char msg[256];
    atree_status_t st = S.tree == NULL ? ATREE_OK : atree_validate(S.tree, msg, sizeof msg);
    if (st != ATREE_OK) {
        reply_err(conn, "ERR %s: %s", atree_strerror(st), msg);
        return;
    }
    reply_ok(conn, "OK");
}

/* Redis pub/sub: SUBSCRIBE atree:<id> | atree:* ... */
static bool channel_id(const char *chan, size_t len, const char *prefix, char sep_all, uint64_t *id)
{
    size_t plen = strlen(prefix);
    if (len <= plen || memcmp(chan, prefix, plen) != 0) {
        return false;
    }
    chan += plen;
    len -= plen;
    if (len == 1 && chan[0] == sep_all) {
        *id = SUB_ALL;
        return true;
    }
    if (len == 3 && memcmp(chan, "all", 3) == 0) {
        *id = SUB_ALL;
        return true;
    }
    return parse_u64(chan, len, id);
}

static void cmd_pubsub_subscribe(struct conn *conn, struct args *args, bool subscribe)
{
    int i;
    if (args_count(args) < 2 && subscribe) {
        reply_err(conn, "ERR usage: SUBSCRIBE atree:<id> | atree:*");
        return;
    }
    if (conn_proto(conn) == PROTO_POSTGRES) {
        reply_err(conn, "ERR use WATCH or LISTEN from Postgres");
        return;
    }
    if (!subscribe && args_count(args) == 1) {
        sub_remove(conn, SUB_RESP, 0, true);
        conn_write_array(conn, 3);
        conn_write_bulk(conn, "unsubscribe", 11);
        conn_write_null(conn);
        conn_write_int(conn, 0);
        return;
    }
    for (i = 1; i < args_count(args); i++) {
        size_t len;
        const char *chan = args_at(args, i, &len);
        uint64_t id;
        if (!channel_id(chan, len, "atree:", '*', &id)) {
            reply_err(conn, "ERR channels are atree:<id> or atree:*");
            return;
        }
        if (subscribe) {
            sub_add(conn, SUB_RESP, id);
        } else {
            sub_remove(conn, SUB_RESP, id, false);
        }
        conn_write_array(conn, 3);
        conn_write_bulk(conn, subscribe ? "subscribe" : "unsubscribe", subscribe ? 9 : 11);
        conn_write_bulk(conn, chan, len);
        conn_write_int(conn, (int64_t)sub_count(conn, SUB_RESP));
    }
}

/* Postgres: WATCH <id>|* streams rows until the client goes away. */
static void cmd_watch(struct conn *conn, struct args *args)
{
    uint64_t id;
    const char *fields[2] = {"id", "event"};
    if (conn_proto(conn) != PROTO_POSTGRES) {
        reply_err(conn, "ERR WATCH is for Postgres clients; Redis clients use SUBSCRIBE");
        return;
    }
    if (args_count(args) != 2 || !arg_id(args, 1, &id)) {
        reply_err(conn, "ERR usage: WATCH <id> | *");
        return;
    }
    pg_write_row_desc(conn, fields, 2);
    sub_add(conn, SUB_PG_WATCH, id);
    /* no CommandComplete: the result set stays open and rows arrive as events match */
}

static void cmd_listen(struct conn *conn, struct args *args, bool listen)
{
    size_t len;
    const char *chan;
    uint64_t id;
    if (conn_proto(conn) != PROTO_POSTGRES) {
        reply_err(conn, "ERR LISTEN is for Postgres clients; Redis clients use SUBSCRIBE");
        return;
    }
    if (args_count(args) != 2) {
        reply_err(conn, "ERR usage: %s atree_<id> | atree_all", listen ? "LISTEN" : "UNLISTEN");
        return;
    }
    chan = args_at(args, 1, &len);
    if (!channel_id(chan, len, "atree_", '*', &id)) {
        reply_err(conn, "ERR channels are atree_<id> or atree_all");
        return;
    }
    if (listen) {
        sub_add(conn, SUB_PG_LISTEN, id);
    } else {
        sub_remove(conn, SUB_PG_LISTEN, id, false);
    }
    pg_write_completef(conn, listen ? "LISTEN" : "UNLISTEN");
    pg_write_ready(conn, 'I');
}

/* ---- HTTP routing ------------------------------------------------------------------ */

static void http_json(struct conn *conn, int code, const char *status, const char *json, size_t len)
{
    conn_write_http_type(conn, code, status, "application/json", json, len);
}

static void http_error(struct conn *conn, int code, const char *status, const char *msg)
{
    struct buf b = {0};
    buf_append(&b, "{\"error\":", 9);
    append_payload_json(&b, msg, strlen(msg));
    buf_append(&b, "}\n", 2);
    http_json(conn, code, status, b.data, b.len);
    buf_clear(&b);
}

static bool path_id(const char *path, size_t len, const char *prefix, uint64_t *id)
{
    size_t plen = strlen(prefix);
    if (len <= plen || memcmp(path, prefix, plen) != 0) {
        return false;
    }
    path += plen;
    len -= plen;
    if (len == 3 && memcmp(path, "all", 3) == 0) {
        *id = SUB_ALL;
        return true;
    }
    return parse_u64(path, len, id);
}

static void cmd_http(struct conn *conn, struct args *args)
{
    size_t mlen;
    size_t plen;
    size_t blen;
    const char *method = args_at(args, 1, &mlen);
    const char *path = args_at(args, 2, &plen);
    const char *body = args_at(args, 4, &blen);
    uint64_t id;
    bool get = argeq_bytes(method, mlen, "get");
    bool post = argeq_bytes(method, mlen, "post");
    bool put = argeq_bytes(method, mlen, "put");
    bool del = argeq_bytes(method, mlen, "delete");

    if (get && (plen == 1 || argeq_bytes(path, plen, "/help"))) {
        conn_write_http_type(conn, 200, "OK", "text/plain", HELP, strlen(HELP));
    } else if (get && argeq_bytes(path, plen, "/stats")) {
        struct buf b = {0};
        stats_json(&b);
        buf_append_byte(&b, '\n');
        http_json(conn, 200, "OK", b.data, b.len);
        buf_clear(&b);
    } else if (get && argeq_bytes(path, plen, "/count")) {
        char text[64];
        size_t n = (size_t)snprintf(text, sizeof text, "{\"count\":%zu}\n",
                                    S.tree != NULL ? atree_count(S.tree) : 0);
        http_json(conn, 200, "OK", text, n);
    } else if (get && argeq_bytes(path, plen, "/validate")) {
        char msg[256];
        atree_status_t st = S.tree == NULL ? ATREE_OK : atree_validate(S.tree, msg, sizeof msg);
        if (st != ATREE_OK) {
            http_error(conn, 500, "Internal Server Error", msg);
        } else {
            http_json(conn, 200, "OK", "{\"valid\":true}\n", 15);
        }
    } else if (get && path_id(path, plen, "/subscribe/", &id)) {
        conn_write_http_stream_head(conn, "text/event-stream");
        conn_write_raw(conn, ": subscribed\n\n", 14);
        sub_add(conn, SUB_SSE, id);
    } else if (post && argeq_bytes(path, plen, "/attributes")) {
        /* body: one "name type" per line */
        const char *p = body;
        const char *e = body + blen;
        size_t added = 0;
        while (p < e) {
            const char *nl = memchr(p, '\n', (size_t)(e - p));
            const char *line_end = nl != NULL ? nl : e;
            const char *sp = memchr(p, ' ', (size_t)(line_end - p));
            if (sp != NULL) {
                const char *t = sp + 1;
                const char *te = line_end;
                while (te > t && (te[-1] == '\r' || te[-1] == ' ')) {
                    te--;
                }
                if (define_attr(p, (size_t)(sp - p), t, (size_t)(te - t)) != 0) {
                    http_error(conn, 400, "Bad Request", "bad attribute line");
                    return;
                }
                added++;
            }
            p = nl != NULL ? nl + 1 : e;
        }
        {
            char text[64];
            size_t n = (size_t)snprintf(text, sizeof text, "{\"defined\":%zu}\n", added);
            http_json(conn, 200, "OK", text, n);
        }
    } else if (post && argeq_bytes(path, plen, "/create")) {
        atree_status_t st = tree_create();
        if (st != ATREE_OK) {
            http_error(conn, 400, "Bad Request", atree_strerror(st));
        } else {
            http_json(conn, 200, "OK", "{\"created\":true}\n", 17);
        }
    } else if ((put || post) && path_id(path, plen, "/queries/", &id) && id != SUB_ALL) {
        atree_error_t err;
        atree_status_t st;
        if (ensure_tree() != ATREE_OK) {
            http_error(conn, 400, "Bad Request", "define attributes first");
            return;
        }
        st = atree_insert(S.tree, id, body, blen, &err);
        if (st != ATREE_OK) {
            char msg[400];
            snprintf(msg, sizeof msg, "%s: %s", atree_strerror(st), err.message);
            http_error(conn, 400, "Bad Request", msg);
        } else {
            http_json(conn, 200, "OK", "{\"subscribed\":true}\n", 20);
        }
    } else if (del && path_id(path, plen, "/queries/", &id) && id != SUB_ALL) {
        atree_status_t st = S.tree == NULL ? ATREE_ERR_NOT_FOUND : atree_delete(S.tree, id);
        if (st != ATREE_OK) {
            http_error(conn, 404, "Not Found", atree_strerror(st));
        } else {
            http_json(conn, 200, "OK", "{\"unsubscribed\":true}\n", 22);
        }
    } else if (post && argeq_bytes(path, plen, "/events")) {
        char err[256];
        uint64_t ns = 0;
        atree_status_t st = ingest(body, blen, err, sizeof err, &ns);
        if (st != ATREE_OK) {
            http_error(conn, 400, "Bad Request", err);
        } else {
            struct buf b = {0};
            char head[96];
            const atree_id_t *m = atree_report_matches(S.rep);
            size_t n = atree_report_count(S.rep);
            size_t i;
            atree_report_stats_t rs;
            atree_report_stats(S.rep, &rs);
            buf_append(&b, head,
                       (size_t)snprintf(head, sizeof head, "{\"count\":%zu,\"matches\":[", n));
            for (i = 0; i < n; i++) {
                buf_append(&b, head,
                           (size_t)snprintf(head, sizeof head, "%s%" PRIu64, i ? "," : "", m[i]));
            }
            buf_append(
                &b, head,
                (size_t)snprintf(head, sizeof head, "],\"time_us\":%.1f,", (double)ns / 1000.0));
            buf_append(&b, head,
                       (size_t)snprintf(head, sizeof head,
                                        "\"nodes_visited\":%" PRIu64
                                        ",\"predicates_evaluated\":%" PRIu64 "}\n",
                                        rs.nodes_visited, rs.predicates_evaluated));
            http_json(conn, 200, "OK", b.data, b.len);
            buf_clear(&b);
        }
    } else {
        http_error(conn, 404, "Not Found", "no such route (GET /help lists them)");
    }
}

/* ---- dispatch --------------------------------------------------------------------- */

void evcommand(struct conn *conn, struct args *args)
{
    if (args_count(args) == 0) {
        return;
    }
    if (arg_is(args, 0, "HTTP")) {
        cmd_http(conn, args);
    } else if (arg_is(args, 0, "PING")) {
        reply_ok(conn, "PONG");
    } else if (arg_is(args, 0, "COMMAND") || arg_is(args, 0, "CLIENT") ||
               arg_is(args, 0, "SELECT")) {
        if (conn_proto(conn) == PROTO_POSTGRES) {
            pg_write_completef(conn, "SELECT 0");
            pg_write_ready(conn, 'I');
        } else {
            conn_write_array(conn, 0); /* redis-cli asks COMMAND DOCS on connect */
        }
    } else if (arg_is(args, 0, "HELLO")) {
        reply_err(conn, "ERR unknown command 'HELLO'"); /* redis-cli falls back to RESP2 */
    } else if (arg_is(args, 0, "HELP")) {
        reply_text(conn, HELP);
    } else if (arg_is(args, 0, "QUIT")) {
        reply_ok(conn, "OK");
        conn_close(conn);
    } else if (arg_is(args, 0, "ATREE.DEFINE") || arg_is(args, 0, "DEFINE")) {
        cmd_define(conn, args);
    } else if (arg_is(args, 0, "ATREE.CREATE") || arg_is(args, 0, "CREATE")) {
        cmd_create(conn);
    } else if (arg_is(args, 0, "ATREE.SUBSCRIBE")) {
        cmd_subscribe_query(conn, args);
    } else if (arg_is(args, 0, "ATREE.UNSUBSCRIBE")) {
        cmd_unsubscribe_query(conn, args);
    } else if (arg_is(args, 0, "ATREE.EVENT") || arg_is(args, 0, "EVENT")) {
        cmd_event(conn, args);
    } else if (arg_is(args, 0, "ATREE.COUNT") || arg_is(args, 0, "COUNT")) {
        reply_int(conn, (int64_t)(S.tree != NULL ? atree_count(S.tree) : 0));
    } else if (arg_is(args, 0, "ATREE.STATS") || arg_is(args, 0, "STATS") ||
               arg_is(args, 0, "INFO")) {
        cmd_stats(conn);
    } else if (arg_is(args, 0, "ATREE.VALIDATE") || arg_is(args, 0, "VALIDATE")) {
        cmd_validate(conn);
    } else if (arg_is(args, 0, "SUBSCRIBE")) {
        cmd_pubsub_subscribe(conn, args, true);
    } else if (arg_is(args, 0, "UNSUBSCRIBE")) {
        cmd_pubsub_subscribe(conn, args, false);
    } else if (arg_is(args, 0, "WATCH")) {
        cmd_watch(conn, args);
    } else if (arg_is(args, 0, "LISTEN")) {
        cmd_listen(conn, args, true);
    } else if (arg_is(args, 0, "UNLISTEN")) {
        cmd_listen(conn, args, false);
    } else {
        size_t len;
        const char *a = args_at(args, 0, &len);
        reply_err(conn, "ERR unknown command '%.*s' (HELP lists them)", (int)(len > 64 ? 64 : len),
                  a);
    }
}

/* ---- main ------------------------------------------------------------------------- */

static void on_listening(void *udata)
{
    (void)udata;
}

static void on_ready(void *udata)
{
    const char **where = udata;
    printf("atreed ready on %s (redis, postgres and http clients on the same port)\n", *where);
    fflush(stdout);
}

static int load_defs(const char *path)
{
    FILE *f = fopen(path, "r");
    char *buf;
    char *line;
    if (f == NULL) {
        fprintf(stderr, "atreed: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    buf = xmalloc(MAX_LINE);
    while ((line = read_line(f, buf)) != NULL) {
        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            continue;
        }
        *tab++ = '\0';
        while (*tab == ' ' || *tab == '\t') {
            tab++;
        }
        if (define_attr(line, strlen(line), tab, strlen(tab)) != 0) {
            fprintf(stderr, "atreed: bad attribute line '%s'\n", line);
        }
    }
    xfree(buf);
    fclose(f);
    if (tree_create() != ATREE_OK) {
        fprintf(stderr, "atreed: could not create the tree from %s\n", path);
        return 1;
    }
    return 0;
}

static int load_exprs(const char *path)
{
    FILE *f = fopen(path, "r");
    char *buf;
    char *line;
    size_t ok = 0;
    size_t bad = 0;
    if (f == NULL) {
        fprintf(stderr, "atreed: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (ensure_tree() != ATREE_OK) {
        fprintf(stderr, "atreed: --exprs needs --defs first\n");
        fclose(f);
        return 1;
    }
    buf = xmalloc(MAX_LINE);
    while ((line = read_line(f, buf)) != NULL) {
        char *tab = strchr(line, '\t');
        uint64_t id;
        if (tab == NULL) {
            bad++;
            continue;
        }
        *tab++ = '\0';
        if (!parse_u64(line, strlen(line), &id) ||
            atree_insert(S.tree, id, tab, SIZE_MAX, NULL) != ATREE_OK) {
            bad++;
        } else {
            ok++;
        }
    }
    xfree(buf);
    fclose(f);
    printf("atreed: %zu queries loaded from %s (%zu rejected)\n", ok, path, bad);
    return 0;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    const char *port = "7777";
    const char *unixsock = "";
    const char *defs_path = NULL;
    const char *exprs_path = NULL;
    char where[256];
    const char *wherep = where;
    struct net_opts opts;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--unixsock") == 0 && i + 1 < argc) {
            unixsock = argv[++i];
        } else if (strcmp(argv[i], "--defs") == 0 && i + 1 < argc) {
            defs_path = argv[++i];
        } else if (strcmp(argv[i], "--exprs") == 0 && i + 1 < argc) {
            exprs_path = argv[++i];
        } else {
            fprintf(
                stderr,
                "usage: atreed [--host H] [--port P] [--unixsock PATH] [--defs F] [--exprs F]\n");
            return 2;
        }
    }
    /* Files load after the options are read, so their order does not matter. */
    if (defs_path != NULL && load_defs(defs_path) != 0) {
        return 1;
    }
    if (exprs_path != NULL && load_exprs(exprs_path) != 0) {
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    xmalloc_init(1);
    if (*unixsock != '\0') {
        snprintf(where, sizeof where, "%s", unixsock);
    } else {
        snprintf(where, sizeof where, "%s:%s", host, port);
    }
    memset(&opts, 0, sizeof opts);
    opts.host = host;
    opts.port = port;
    opts.tlsport = "";
    opts.unixsock = unixsock;
    opts.reuseport = false;
    opts.tcpnodelay = true;
    opts.keepalive = true;
    opts.quickack = false;
    opts.backlog = 1024;
    opts.queuesize = 128;
    opts.nthreads = 1; /* one loop: the tree needs no lock and pushes are same-thread */
    opts.maxconns = 4096;
    opts.nowarmup = true;
    opts.nouring = true;
    opts.udata = &wherep;
    opts.listening = on_listening;
    opts.ready = on_ready;
    opts.data = evdata;
    opts.opened = evopened;
    opts.closed = evclosed;
    net_main(&opts);
    return 0;
}
