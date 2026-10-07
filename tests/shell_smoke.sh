#!/bin/sh
# Smoke test for tools/atree_shell: a scripted stdin session compared with
# the expected transcript (timing, memory and DOT lines filtered), then a
# server with two clients over a Unix socket, checking that an event from
# one client notifies the other's continuous query.
set -u
SHELL_BIN="$1"
DIR=$(dirname "$0")
TMP=$(mktemp -d 2>/dev/null || mktemp -d -t atree)
trap 'rm -rf "$TMP"' EXIT
status=0

"$SHELL_BIN" < "$DIR/shell_smoke.in" |
    grep -v -e '^TIME ' -e '^bytes_' -e '^digraph' -e '^}' -e '^  ' > "$TMP/out.txt"
if ! diff -u "$DIR/shell_smoke.expected" "$TMP/out.txt"; then
    echo "shell smoke: stdin transcript differs"
    status=1
fi

SOCK="$TMP/atree.sock"
"$SHELL_BIN" --listen "$SOCK" > "$TMP/server.log" 2>&1 &
SERVER=$!
i=0
while [ ! -S "$SOCK" ] && [ $i -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done
# client A subscribes and stays connected (its stdin is a FIFO we hold open)
FIFO="$TMP/a.in"
mkfifo "$FIFO"
"$SHELL_BIN" --connect "$SOCK" < "$FIFO" > "$TMP/a.out" 2>&1 &
CLIENT_A=$!
exec 3> "$FIFO"
printf 'DEFINE price int\nDEFINE country string\nSUBSCRIBE 10 price > 10 and country = "US"\n' >&3
sleep 0.3
# client B ingests two events and leaves
printf 'EVENT price=12;country="US"\nEVENT price=1\n' | "$SHELL_BIN" --connect "$SOCK" > "$TMP/b.out" 2>&1
sleep 0.3
printf 'COUNT\nQUIT\n' >&3
exec 3>&-
wait $CLIENT_A
kill $SERVER 2>/dev/null
wait $SERVER 2>/dev/null

if ! grep -q '^NOTIFY 10 price=12;country="US"$' "$TMP/a.out"; then
    echo "shell smoke: client A did not receive the notification"; cat "$TMP/a.out"; status=1
fi
if ! grep -q '^MATCH 1: 10$' "$TMP/b.out" || ! grep -q '^MATCH 0$' "$TMP/b.out"; then
    echo "shell smoke: client B's match lines are wrong"; cat "$TMP/b.out"; status=1
fi
if ! grep -q '^OK 1$' "$TMP/a.out"; then
    echo "shell smoke: COUNT after the other client left should be 1"; cat "$TMP/a.out"; status=1
fi
if [ $status -eq 0 ]; then
    echo "shell smoke: ok"
fi
exit $status
