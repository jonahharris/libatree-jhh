#!/usr/bin/env python3
"""Protocol tests for atreed: raw RESP, HTTP and Postgres clients (no
dependencies) against a server started on a free port. Exits nonzero on
the first failure and prints what it saw."""
import http.client
import os
import socket
import struct
import subprocess
import sys
import time


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Resp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.settimeout(5)
        self.buf = b""

    def send(self, *args):
        out = b"*%d\r\n" % len(args)
        for a in args:
            a = a.encode() if isinstance(a, str) else a
            out += b"$%d\r\n%s\r\n" % (len(a), a)
        self.s.sendall(out)

    def _line(self):
        while b"\r\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise EOFError
            self.buf += d
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def read(self):
        line = self._line()
        t, rest = line[:1], line[1:]
        if t == b"+":
            return rest.decode()
        if t == b"-":
            return ("ERR", rest.decode())
        if t == b":":
            return int(rest)
        if t == b"$":
            n = int(rest)
            if n < 0:
                return None
            while len(self.buf) < n + 2:
                self.buf += self.s.recv(65536)
            data, self.buf = self.buf[:n], self.buf[n + 2:]
            return data.decode()
        if t == b"*":
            return [self.read() for _ in range(int(rest))]
        raise ValueError(line)

    def call(self, *args):
        self.send(*args)
        return self.read()

    def close(self):
        self.s.close()


class Pg:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.settimeout(5)
        self.buf = b""
        params = b"user\0test\0database\0test\0\0"
        msg = struct.pack("!ii", 8 + len(params), 196608) + params
        self.s.sendall(msg)
        msgs = self.read_until(b"Z")
        assert msgs[0][0] == b"R", msgs

    def _msg(self):
        while len(self.buf) < 5:
            d = self.s.recv(65536)
            if not d:
                raise EOFError
            self.buf += d
        t = self.buf[:1]
        n = struct.unpack("!i", self.buf[1:5])[0]
        while len(self.buf) < 1 + n:
            self.buf += self.s.recv(65536)
        body, self.buf = self.buf[5:1 + n], self.buf[1 + n:]
        return t, body

    def read_until(self, kind):
        msgs = []
        while True:
            m = self._msg()
            msgs.append(m)
            if m[0] == kind:
                return msgs

    def query(self, sql):
        q = sql.encode() + b"\0"
        self.s.sendall(b"Q" + struct.pack("!i", 4 + len(q)) + q)
        return self.read_until(b"Z")

    def rows(self, msgs):
        out = []
        for t, body in msgs:
            if t == b"D":
                ncols = struct.unpack("!h", body[:2])[0]
                p = 2
                row = []
                for _ in range(ncols):
                    n = struct.unpack("!i", body[p:p + 4])[0]
                    p += 4
                    row.append(body[p:p + n].decode())
                    p += n
                out.append(row)
        return out

    def error(self, msgs):
        for t, body in msgs:
            if t == b"E":
                for field in body.split(b"\0"):
                    if field[:1] == b"M":
                        return field[1:].decode()
        return None


def http_req(port, method, path, body=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    c.request(method, path, body=body)
    r = c.getresponse()
    data = r.read().decode()
    c.close()
    return r.status, data


def check(cond, what, *seen):
    if not cond:
        print("FAIL:", what, *seen)
        sys.exit(1)


def main():
    exe = sys.argv[1]
    port = free_port()
    srv = subprocess.Popen([exe, "--port", str(port)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        r = Resp(port)
        check(r.call("PING") == "PONG", "ping")
        check(r.call("ATREE.DEFINE", "price", "int") == "OK", "define")
        check(r.call("ATREE.DEFINE", "country", "string") == "OK", "define")
        check(r.call("ATREE.DEFINE", "tags", "string_list") == "OK", "define")
        check(r.call("ATREE.DEFINE", "score", "float") == "OK", "define")
        check(r.call("ATREE.CREATE") == "OK", "create")
        check(r.call("ATREE.SUBSCRIBE", "7", "price > 10 and country = 'US'") == "OK", "subscribe 7")
        check(r.call("ATREE.SUBSCRIBE", "8", "tags one of ['a', 'b']") == "OK", "subscribe 8")
        bad = r.call("ATREE.SUBSCRIBE", "9", "price >")
        check(isinstance(bad, tuple) and "syntax error" in bad[1], "bad expression", bad)
        check(r.call("ATREE.EVENT", 'price=12;country="US"') == [7], "event line", )
        check(r.call("ATREE.EVENT", '{"price": 12, "country": "US", "tags": ["b"]}') == [7, 8], "event json")
        check(r.call("ATREE.EVENT", "price", "5", "tags", '["a"]') == [8], "event kv")
        bad = r.call("ATREE.EVENT", '{"price": "oops"}')
        check(isinstance(bad, tuple) and "type mismatch" in bad[1], "bad json type", bad)
        bad = r.call("ATREE.EVENT", "price=xx")
        check(isinstance(bad, tuple) and "bad item" in bad[1], "bad line value", bad)
        check(r.call("ATREE.COUNT") == 2, "count")
        check("nodes:" in r.call("ATREE.STATS"), "stats")
        check(r.call("ATREE.VALIDATE") == "OK", "validate")

        # Redis pub/sub delivery to another connection, then its departure.
        sub = Resp(port)
        check(sub.call("SUBSCRIBE", "atree:7") == ["subscribe", "atree:7", 1], "subscribe channel")
        check(r.call("ATREE.EVENT", '{"price": 50, "country": "US"}') == [7], "event for sub")
        msg = sub.read()
        check(msg == ["message", "atree:7", '{"price": 50, "country": "US"}'], "pubsub message", msg)
        check(r.call("ATREE.EVENT", '{"price": 1, "tags": ["a"]}') == [8], "event not for sub")
        allsub = Resp(port)
        check(allsub.call("SUBSCRIBE", "atree:*") == ["subscribe", "atree:*", 1], "subscribe all")
        check(r.call("ATREE.EVENT", '{"price": 51, "country": "US", "tags": ["a"]}') == [7, 8], "event two")
        check(sub.read()[2].startswith('{"price": 51'), "sub gets 7")
        m1 = allsub.read()
        m2 = allsub.read()
        check(m1[1] == "atree:*" and m2[1] == "atree:*", "all gets both", m1, m2)
        check(m1[2].startswith('{"id":7,"event":{"price": 51') and m2[2].startswith('{"id":8,"event":{'),
              "catch-all payload carries the id", m1, m2)
        check("subscribers:2" in r.call("ATREE.STATS"), "two subscribers")
        sub.close()
        allsub.close()
        time.sleep(0.2)
        check("subscribers:0" in r.call("ATREE.STATS"), "subscribers gone after close")

        # HTTP
        st, body = http_req(port, "GET", "/count")
        check(st == 200 and body.strip() == '{"count":2}', "http count", body)
        st, body = http_req(port, "PUT", "/queries/10", "score between 0.5 and 1.5")
        check(st == 200 and "subscribed" in body, "http put query", body)
        st, body = http_req(port, "PUT", "/queries/11", "score <")
        check(st == 400 and "syntax error" in body, "http bad query", body)
        st, body = http_req(port, "POST", "/events", '{"score": 1.0}')
        check(st == 200 and '"matches":[10]' in body, "http event", body)
        st, body = http_req(port, "POST", "/events", "score=abc")
        check(st == 400 and "bad item" in body, "http bad event", body)
        st, body = http_req(port, "GET", "/stats")
        check(st == 200 and '"subscriptions":3' in body, "http stats", body)
        st, body = http_req(port, "GET", "/validate")
        check(st == 200 and "true" in body, "http validate", body)
        st, body = http_req(port, "GET", "/nope")
        check(st == 404, "http 404", st)
        # SSE: raw socket so we can read the stream incrementally
        sse = socket.create_connection(("127.0.0.1", port))
        sse.settimeout(5)
        sse.sendall(b"GET /subscribe/10 HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n")
        head = b""
        while b": subscribed\n\n" not in head:
            head += sse.recv(65536)
        check(b"text/event-stream" in head, "sse headers", head)
        check(r.call("ATREE.EVENT", '{"score": 1.2}') == [10], "event for sse")
        frame = b""
        while b"\n\n" not in frame:
            frame += sse.recv(65536)
        check(frame == b'event: match\ndata: {"id":10,"event":{"score": 1.2}}\n\n', "sse frame", frame)
        sse.close()
        st, body = http_req(port, "DELETE", "/queries/10")
        check(st == 200, "http delete", body)
        st, body = http_req(port, "DELETE", "/queries/10")
        check(st == 404, "http delete again", body)

        # Postgres: simple queries, doubled quotes, WATCH streaming, LISTEN
        pg = Pg(port)
        msgs = pg.query("ATREE.COUNT")
        check(pg.rows(msgs) == [["2"]], "pg count", pg.rows(msgs))
        msgs = pg.query("ATREE.SUBSCRIBE 20 'country = ''CA'' and price >= 3'")
        check(pg.rows(msgs) == [["OK"]], "pg subscribe", pg.rows(msgs), pg.error(msgs))
        msgs = pg.query("ATREE.EVENT 'country=\"CA\";price=3'")
        check(pg.rows(msgs) == [["20"]], "pg event rows", pg.rows(msgs))
        msgs = pg.query("ATREE.SUBSCRIBE 21 'price >'")
        check(pg.error(msgs) and "syntax error" in pg.error(msgs), "pg bad expression", msgs)
        watcher = Pg(port)
        q = b"WATCH 20\0"
        watcher.s.sendall(b"Q" + struct.pack("!i", 4 + len(q)) + q)
        t, body = watcher._msg()
        check(t == b"T", "watch row description", t)
        listener = Pg(port)
        msgs = listener.query("LISTEN atree_all")
        check(any(t == b"C" for t, _ in msgs), "listen complete", msgs)
        check(r.call("ATREE.EVENT", 'country="CA";price=9') == [20], "event for watchers")
        t, body = watcher._msg()
        check(t == b"D" and watcher.rows([(t, body)]) == [["20", 'country="CA";price=9']], "watch row", t, body)
        t, body = listener._msg()
        check(t == b"A" and b"atree_all\0" in body and body.endswith(b'{"id":20,"event":"country=\\"CA\\";price=9"}\0'),
              "pg notification", t, body)
        watcher.s.close()
        listener.s.close()
        time.sleep(0.2)
        check("subscribers:0" in r.call("ATREE.STATS"), "pg subscribers gone")
        check(r.call("QUIT") == "OK", "quit")
        print("atreed tests: ok")
    finally:
        srv.terminate()
        try:
            out = srv.communicate(timeout=3)[0].decode()
        except subprocess.TimeoutExpired:
            srv.kill()
            out = ""
        if "--verbose" in sys.argv:
            print(out)


if __name__ == "__main__":
    main()
