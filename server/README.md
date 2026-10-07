# atreed

A continuous-query server over libatree. One A-Tree is served to Redis,
Postgres and HTTP clients on the same port: the first bytes of a connection
pick the protocol, each protocol's request is parsed into one argument
list, and every command is written once. The networking core is vendored
from [pogocache](https://github.com/tidwall/pogocache) (MIT, see
`deps/pogocache/MODIFICATIONS.md`); the library is used only through
`include/atree.h`.

```sh
make -C server            # builds server/build/atreed (and the library, release mode)
make -C server check      # protocol tests: raw RESP, HTTP and Postgres clients (python3)
server/build/atreed --port 7777 [--defs F] [--exprs F] [--unixsock PATH]
```

## Commands

Case-insensitive; the `ATREE.` prefix is optional over RESP and Postgres.

| command | effect |
|---|---|
| `ATREE.DEFINE name type` | add an attribute (`bool int float string int_list string_list`) |
| `ATREE.CREATE` | (re)build the tree over the defined attributes; drops queries |
| `ATREE.SUBSCRIBE id expr` | register a continuous query |
| `ATREE.UNSUBSCRIBE id` | remove it |
| `ATREE.EVENT payload` | ingest an event; replies with the matched ids |
| `ATREE.COUNT`, `ATREE.STATS`, `ATREE.VALIDATE`, `HELP`, `PING`, `QUIT` | |

The payload is either the `bench_file` line format, `a=v;b=v;...`, or a
JSON object (`{"price": 12, "tags": ["a", "b"]}`), typed by the schema.
From a Redis client `ATREE.EVENT k v k v ...` also works.

## Delivery

A matched event goes to every subscriber of its query id, and to every
catch-all subscriber, in that subscriber's protocol. Subscriptions end
with the connection.

- **Redis**: `SUBSCRIBE atree:7` (or `atree:*`); each match arrives as a
  standard pub/sub `message` whose payload is the event text. Any Redis
  client library works unchanged.
- **Postgres**: `WATCH 7` (or `WATCH *`) answers with a row description and
  then streams one `(id, event)` row per match, flushed as it happens,
  without ever completing; libpq single-row mode and pgx read the rows as
  they arrive. `LISTEN atree_7` (or `atree_all`) delivers each match as an
  asynchronous notification instead.
- **HTTP**: `GET /subscribe/7` (or `/all`) is a `text/event-stream`; each
  match is `event: match` with `data: {"id": 7, "event": ...}`, the event
  embedded raw when it was JSON and as a string otherwise.

## Examples

```sh
redis-cli -p 7777 ATREE.DEFINE price int
redis-cli -p 7777 ATREE.DEFINE country string
redis-cli -p 7777 ATREE.SUBSCRIBE 7 "price > 10 and country = 'US'"
redis-cli -p 7777 SUBSCRIBE atree:7 &
redis-cli -p 7777 ATREE.EVENT '{"price": 12, "country": "US"}'

psql -h 127.0.0.1 -p 7777 -c "ATREE.SUBSCRIBE 8 'country = ''CA'''"
psql -h 127.0.0.1 -p 7777 -c "ATREE.EVENT 'country=\"CA\"'"

curl -XPUT localhost:7777/queries/9 -d "price < 3"
curl -N localhost:7777/subscribe/9 &
curl localhost:7777/events -d '{"price": 1}'
curl localhost:7777/stats
```

HTTP routes: `POST /attributes` (body: `name type` lines), `POST /create`,
`PUT|POST /queries/<id>` (body: expression), `DELETE /queries/<id>`,
`POST /events` (body: payload; answers with the matches, time and search
counters), `GET /subscribe/<id>|all`, `GET /stats`, `GET /count`,
`GET /validate`, `GET /help`.

## Design notes

The server runs one event-loop thread (epoll on Linux, kqueue on macOS).
Searches take tens to hundreds of microseconds and dominate the per-event
cost, the tree then needs no lock, and a push to another connection is a
write into its buffer followed by a flush on the same thread. Pogocache's
loop has no cross-thread wake, so going multi-threaded would mean adding
per-thread mailboxes to the vendored `net.c`; the single loop is the
starting point, not a ceiling. A slow subscriber stalls the flush (the
vendored write loop retries on `EAGAIN`), which is acceptable for a
playground and the first thing to change for anything else. No TLS, no
authentication.
