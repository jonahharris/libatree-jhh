# atreed

A continuous-query server over libatree. One A-Tree is served to Redis,
Postgres and HTTP clients on the same port: the first bytes of a connection
pick the protocol, each protocol's request is parsed into one argument
list, and every command is written once. The networking core is vendored
from [pogocache](https://github.com/tidwall/pogocache) (MIT, see
`deps/pogocache/MODIFICATIONS.md`); the library is used only through
`include/atree.h`, and the event line format is parsed by the shared
`bench/bench_format.h`.

```sh
make -C server            # builds server/build/atreed (and the library, release mode)
make -C server check      # protocol tests: raw RESP, HTTP and Postgres clients (python3)
server/build/atreed [--host H] [--port P] [--unixsock PATH] [--defs F] [--exprs F]
```

Defaults: host `127.0.0.1`, port `7777`. `--defs` loads attribute
definitions (`name<TAB>type` per line) and creates the tree; `--exprs`
loads queries (`id<TAB>expression` per line); both files skip `#` lines,
and `--exprs` requires `--defs`. Nothing is persisted: a restart starts
with an empty schema unless the files are given again.

Terms: a *query* is a subscription in the library's sense, an expression
registered under an id. A *subscriber* is a connection that asked to be
told about a query's matches. The two are independent: a process that only
wants notifications never needs to know the expression.

## Commands

Case-insensitive. The `ATREE.` prefix is optional over RESP and Postgres
except for `SUBSCRIBE` and `UNSUBSCRIBE`, whose bare forms are Redis
pub/sub.

| command | effect |
|---|---|
| `ATREE.DEFINE name type` | add an attribute (`bool int float string int_list string_list`); once the tree exists, takes effect at the next `CREATE` |
| `ATREE.CREATE` | (re)build the tree over the defined attributes; drops every query, keeps the subscribers |
| `ATREE.SUBSCRIBE id expr` | register a continuous query (creates the tree if needed) |
| `ATREE.UNSUBSCRIBE id` | remove it |
| `ATREE.EVENT payload` | ingest an event; replies with the matched ids |
| `ATREE.COUNT` | number of queries |
| `ATREE.STATS` (or `INFO`) | index statistics, events and deliveries, subscriber count |
| `ATREE.VALIDATE` | the library's structural self-check |
| `HELP`, `PING`, `QUIT` | help text, `PONG`, close the connection |

The payload is either the line format, `a=v;b=v;...` with values `true`,
`false`, `12`, `1.5`, `"text"`, `[1, 2]` or `["a", "b"]`, or a JSON object
(`{"price": 12, "tags": ["a", "b"]}`), typed by the schema: a JSON `null`
leaves the attribute undefined, an integer is accepted for a float
attribute, arrays must be homogeneous, and an attribute the payload omits
is undefined. From a Redis client `ATREE.EVENT k v k v ...` also works.
The tree is created implicitly by the first `SUBSCRIBE` or `EVENT`. Events
are processed one at a time on the loop thread. At most 4096 attributes and
4096 connections.

Errors come back in each protocol's native form: a RESP `-ERR ...` line, a
Postgres `ErrorResponse`, or an HTTP 400/404/500 with `{"error": "..."}`.
Redis clients' handshake commands (`COMMAND`, `CLIENT`, `SELECT`) are
accepted as no-ops and `HELLO` is refused, so `redis-cli` and client
libraries connect unchanged.

## Delivery

A matched event goes to every subscriber of its query id and to every
catch-all subscriber, in that subscriber's protocol. A per-id subscriber
receives the event text once per event; a catch-all subscriber receives one
message per matched id, each carrying the id. Delivery subscriptions end
with the connection; queries do not (unlike `atree_shell`, where a client's
queries are deleted when it disconnects).

- **Redis**: `SUBSCRIBE atree:7`; each match is a standard pub/sub `message`
  whose payload is the event text. `SUBSCRIBE atree:*` (or `atree:all`) is
  the catch-all; its payload is `{"id":7,"event":...}` with the event
  embedded raw when it was JSON and as a string otherwise. `UNSUBSCRIBE`
  works as in Redis. Any Redis client library works unchanged.
- **Postgres**: `WATCH 7` (or `WATCH *`) answers with a row description and
  then streams one `(id, event)` row per match, flushed as it happens,
  without ever completing; libpq single-row mode and pgx read the rows as
  they arrive, but `psql` itself shows nothing until the connection ends.
  From `psql` use `LISTEN atree_7` (or `atree_all`), which delivers each
  match as an asynchronous notification; `atree_all` notifications carry
  the `{"id":...,"event":...}` payload. `UNLISTEN` removes it.
- **HTTP**: `GET /subscribe/7` (or `/all`) is a `text/event-stream`; each
  match is `event: match` followed by `data: {"id":7,"event":...}`.

## Examples

```sh
redis-cli -p 7777 ATREE.DEFINE price int
redis-cli -p 7777 ATREE.DEFINE country string
redis-cli -p 7777 ATREE.SUBSCRIBE 7 "price > 10 and country = 'US'"
redis-cli -p 7777 SUBSCRIBE atree:7 &
redis-cli -p 7777 ATREE.EVENT '{"price": 12, "country": "US"}'

psql -h 127.0.0.1 -p 7777 -c "ATREE.SUBSCRIBE 8 'country = ''CA'''"   # any user and database
psql -h 127.0.0.1 -p 7777 -c "ATREE.EVENT 'country=\"CA\"'"

curl -XPUT localhost:7777/queries/9 -d "price < 3"
curl -N localhost:7777/subscribe/9 &
curl localhost:7777/events -d '{"price": 1}'
curl localhost:7777/stats
```

HTTP routes: `POST /attributes` (body: `name type` lines), `POST /create`,
`PUT|POST /queries/<id>` (body: expression), `DELETE /queries/<id>`,
`POST /events` (body: payload; answers `{"count", "matches", "time_us",
"nodes_visited", "predicates_evaluated"}`), `GET /subscribe/<id>|all`,
`GET /stats`, `GET /count`, `GET /validate`, `GET /help` (also `GET /`).

## Design notes

The server runs one event-loop thread (epoll on Linux, kqueue on macOS).
One thread owns the tree, so it needs no lock, and a push to another
connection is a write into that connection's buffer followed by a flush on
the same thread. A search takes tens to hundreds of microseconds and is the
bulk of the per-event cost. Pogocache's loop has no cross-thread wake, so
going multi-threaded would mean adding per-thread mailboxes to the vendored
`net.c`; the single loop is the starting point, not a ceiling.

Known limits: a slow subscriber stalls the event loop, and with it every
client on every protocol, because the vendored write loop retries on
`EAGAIN` on the loop thread (acceptable for a playground, the first thing
to change for anything else); no TLS, no authentication; no persistence.
