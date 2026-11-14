#!/usr/bin/env python3
"""Test server for the DuncOS Web browser: serves this folder and a few dynamic pages.
Usage: python3 server.py [port]   (default 8090; in QEMU user networking the host is http://10.0.2.2:PORT/)
  /echo            shows the fields of a GET or POST form (decoded as UTF-8)
  /login           POST user=...: sets a persistent cookie and a session cookie, then redirects (303) to /welcome
  /welcome         shows the cookies the browser sent back
  /cookie/set?k=v  sets cookie k=v (Max-Age one day, Path=/); /cookie/clear expires it; /cookie/show lists them
  /cookie/evil     tries to set cookies for public suffixes and other domains (all must be refused)
  /gzip.html       gzip.html sent with Content-Encoding: gzip; /gzip-chunked.html also chunked
  /latin1.html     a page in ISO-8859-1
  /big.html        a page of about 400 KB
"""
import gzip, html, os, sys, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.abspath(__file__))
TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css", ".png": "image/png", ".jpg": "image/jpeg",
         ".gif": "image/gif", ".bmp": "image/bmp", ".txt": "text/plain; charset=utf-8"}

def page(title, body):
    return ("<!doctype html><html><head><meta charset=utf-8><title>%s</title><link rel=stylesheet href=/style.css></head>"
            "<body><div class=container><h1>%s</h1>%s<p><a href=/index.html>Back to the tests</a></p></div></body></html>" % (title, title, body)).encode()

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, fmt, *a):
        sys.stderr.write("%s %s\n" % (self.command, self.path))
    def send(self, code, body, ctype="text/html; charset=utf-8", headers=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers: self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD": self.wfile.write(body)
    def cookies(self):
        c = {}
        for part in self.headers.get("Cookie", "").split(";"):
            if "=" in part:
                k, v = part.strip().split("=", 1); c[k] = v
        return c
    def fields(self, body):
        q = urllib.parse.parse_qsl(body, keep_blank_values=True, encoding="utf-8", errors="replace")
        rows = "".join("<tr><td><code>%s</code></td><td>%s</td></tr>" % (html.escape(k), html.escape(v)) for k, v in q)
        return "<table class=grid><tr><th>Field</th><th>Value</th></tr>%s</table>" % rows
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        data = self.rfile.read(n).decode("ascii", "replace")
        u = urllib.parse.urlparse(self.path)
        if u.path == "/login":
            user = dict(urllib.parse.parse_qsl(data)).get("user", "nobody")
            self.send(303, b"", headers=[("Location", "/welcome"),
                                          ("Set-Cookie", "user=%s; Max-Age=86400; Path=/; HttpOnly" % urllib.parse.quote(user)),
                                          ("Set-Cookie", "visit=session1; Path=/")])
            return
        body = "<p>Received a <b>POST</b> with Content-Type <code>%s</code> and %d bytes:</p><pre>%s</pre>%s" % (
            html.escape(self.headers.get("Content-Type", "")), n, html.escape(data), self.fields(data))
        self.send(200, page("Form received", body))
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        p = u.path
        if p == "/echo":
            self.send(200, page("Search results", "<p>Query string: <code>%s</code></p>%s" % (html.escape(u.query), self.fields(u.query))))
        elif p == "/welcome":
            c = self.cookies()
            who = urllib.parse.unquote(c.get("user", ""))
            body = ("<p class=ok>Signed in as <b>%s</b>.</p>" % html.escape(who)) if who else "<p class=bad>Not signed in (no cookie).</p>"
            body += "<p>Cookie header: <code>%s</code></p>" % html.escape(self.headers.get("Cookie", "(none)"))
            self.send(200, page("Welcome", body))
        elif p == "/cookie/set":
            hs = [("Set-Cookie", "%s=%s; Max-Age=86400; Path=/" % (k, v)) for k, v in urllib.parse.parse_qsl(u.query)]
            self.send(302, b"", headers=hs + [("Location", "/cookie/show")])
        elif p == "/cookie/clear":
            hs = [("Set-Cookie", "%s=; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Path=/" % k) for k in self.cookies()]
            self.send(302, b"", headers=hs + [("Location", "/cookie/show")])
        elif p == "/cookie/evil":
            hs = [("Set-Cookie", "evil1=1; Domain=com; Path=/"), ("Set-Cookie", "evil2=1; Domain=.co.uk; Path=/"),
                  ("Set-Cookie", "evil3=1; Domain=example.org; Path=/"), ("Set-Cookie", "evil4=1; Secure; Path=/"),
                  ("Set-Cookie", "pathonly=1; Path=/elsewhere")]
            self.send(302, b"", headers=hs + [("Location", "/cookie/show")])
        elif p == "/cookie/show":
            c = self.cookies()
            rows = "".join("<li><code>%s</code> = <code>%s</code></li>" % (html.escape(k), html.escape(v)) for k, v in c.items())
            self.send(200, page("Cookies sent", "<p>%d cookie(s) came back:</p><ul>%s</ul>" % (len(c), rows or "<li>none</li>")))
        elif p in ("/gzip.html", "/gzip-chunked.html"):
            raw = open(os.path.join(ROOT, "gzip.html"), "rb").read()
            z = gzip.compress(raw)
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Encoding", "gzip")
            if p == "/gzip.html":
                self.send_header("Content-Length", str(len(z))); self.end_headers(); self.wfile.write(z)
            else:
                self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
                for i in range(0, len(z), 700):
                    part = z[i:i + 700]
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
                self.wfile.write(b"0\r\n\r\n")
            sys.stderr.write("  (gzip: %d bytes -> %d)\n" % (len(raw), len(z)))
        elif p == "/latin1.html":
            body = open(os.path.join(ROOT, "utf8.html"), encoding="utf-8").read()
            body = body.replace("charset=utf-8", "charset=iso-8859-1").replace("UTF-8 page", "ISO-8859-1 page").replace("€", "&euro;")
            self.send(200, body.encode("latin-1", "replace"), "text/html; charset=iso-8859-1")
        elif p == "/big.html":
            parts = ["<!doctype html><html><head><meta charset=utf-8><title>A big page</title><link rel=stylesheet href=/style.css>"
                     "<style>.row:nth-child(odd){background:#F1F5F9} td{padding:2px 8px} .n{color:#888}</style></head><body><div class=container>"
                     "<h1>A big page</h1><p>Many paragraphs, links and a long table, to check that layout and scrolling stay quick.</p>"]
            for i in range(1500):
                parts.append("<p><span class=n>%d.</span> Lorem ipsum dolor sit amet, <a href=#p%d>consectetur</a> adipiscing elit, "
                             "<b>sed do eiusmod</b> tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam.</p>" % (i, i))
            parts.append("<table>")
            for i in range(600):
                parts.append("<tr class=row id=p%d><td>%d</td><td><a href=/echo?row=%d>Row %d</a></td><td>Some cell text that is a little longer</td><td align=right>%d.%02d</td></tr>" % (i, i, i, i, i * 7, i % 100))
            parts.append("</table></div></body></html>")
            self.send(200, "".join(parts).encode())
        elif p == "/redirect":
            self.send(301, b"", headers=[("Location", "/layout.html")])
        else:
            f = os.path.normpath(os.path.join(ROOT, p.lstrip("/") or "index.html"))
            if os.path.isdir(f): f = os.path.join(f, "index.html")
            if not f.startswith(ROOT) or not os.path.isfile(f):
                self.send(404, page("Not found", "<p>No such page: <code>%s</code></p>" % html.escape(p))); return
            ext = os.path.splitext(f)[1]
            self.send(200, open(f, "rb").read(), TYPES.get(ext, "application/octet-stream"))

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8090
    ThreadingHTTPServer(("0.0.0.0", port), H).serve_forever()
