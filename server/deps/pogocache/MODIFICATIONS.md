# Vendored from pogocache

Source: https://github.com/tidwall/pogocache, commit 9ca1b8d, MIT license
(see LICENSE). Only the networking and protocol units are taken: net, buf,
conn, parse, resp, postgres, http, hashmap, util, args, sys, xmalloc,
stats, tls (compiled as stubs with NOOPENSSL), uring (NOURING). The cache
itself, memcache, save, monitor, lz4 and main are not.

Changes made here, each marked `atreed` in the code:

- `parse.c`, `parse.h`: the memcache protocol is removed; a first line of
  text that is not RESP, HTTP or Postgres is treated as RESP+telnet.
- `conn.c`, `conn.h`: memcache and the help page removed; `PROTO_MEMCACHE`
  gone. Added a `streaming` flag so an HTTP response can stay open
  (Server-Sent Events), `conn_flush`, `conn_write_http_stream_head`,
  `conn_write_http_type`, and the `evclosed_hook` call so the server can
  drop a closing connection's subscriptions.
- `net.c`, `net.h`: `net_conn_flush`, which writes a connection's pending
  output immediately. Used for pushes to connections other than the one
  whose data callback is running; same event-loop thread only.
- `http.c`: the request is handed to the command layer as
  `HTTP method path query body accept` instead of being mapped to
  get/set/del; bearer-token auth removed.
- `postgres.c`: `arg_append_unescape_simplestr` looped to `str2len`
  (always 0) instead of `slen`, so every quoted string containing a doubled
  quote arrived empty. Fixed; worth reporting upstream.
