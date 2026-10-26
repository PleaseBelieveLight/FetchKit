"""Real HTTP protocol scenarios, including malformed ranges and streamed >4 GiB."""

from urllib.parse import urlsplit

from http_fixture import CONTENT, FixtureHandler

CHANGED = bytes((i + 7) % 251 for i in range(len(CONTENT)))
LARGE_SIZE = 4 * 1024**3 + 65539
LARGE_BLOCK = bytes(i % 251 for i in range(65536))
ETAG = '"m1-v1"'


class M1Handler(FixtureHandler):
    def do_HEAD(self):
        with self.server.requests_lock:
            self.server.requests.append({"method": "HEAD", "path": urlsplit(self.path).path})
        self.respond(405, b"")

    def respond(self, status, body, extra=(), etag=ETAG, length=None, chunked=False, stall=False, truncated=False):
        self.send_response(status)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(len(body) if length is None else length))
        if etag:
            self.send_header("ETag", etag)
        self.send_header("Connection", "close")
        for key, value in extra:
            self.send_header(key, value)
        self.end_headers()
        if chunked and body:
            self.wfile.write(f"{len(body):x}\r\n".encode() + body + b"\r\n")
        else:
            self.wfile.write(body)
        self.wfile.flush()
        if stall:
            self.wait_connected(10000)
        if chunked and not stall and not truncated:
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        self.close_connection = True

    def handle_get(self, path, query):
        if not path.startswith("/m1/"):
            return super().handle_get(path, query)
        mode = path[4:]
        with self.server.requests_lock:
            counts = getattr(self.server, "mode_counts", {})
            count = counts[path] = counts.get(path, 0) + 1
            self.server.mode_counts = counts
        if mode == "large":
            self.send_response(200)
            self.send_header("Content-Length", str(LARGE_SIZE))
            self.send_header("ETag", ETAG)
            self.send_header("Connection", "close")
            self.end_headers()
            remaining = LARGE_SIZE
            while remaining and not self.server.stopping.is_set():
                block = LARGE_BLOCK[:min(remaining, len(LARGE_BLOCK))]
                self.wfile.write(block)
                remaining -= len(block)
            self.close_connection = True
            return
        if mode == "empty": return self.respond(200, b"")
        if mode == "unknown-close":
            self.send_response(200); self.send_header("Connection", "close"); self.end_headers()
            self.wfile.write(CONTENT); self.wfile.flush(); self.close_connection = True
            return
        if mode in ("unknown", "unknown-limit"): return self.respond(200, CONTENT, chunked=True)
        if mode == "gzip": return self.respond(200, b"must-not-write", [("Content-Encoding", "gzip")])
        if mode == "conflicting-length": return self.respond(200, b"", [("Content-Length", "123")], length=124)
        if mode == "overflow-length": return self.respond(200, b"", length=2**63)
        if mode == "length-mismatch": return self.respond(200, CONTENT)
        if mode == "not-found": return self.respond(404, b"must-not-write")
        if mode == "fresh-416": return self.respond(416, b"must-not-write", [("Content-Range", f"bytes */{len(CONTENT)}")])
        if mode == "unavailable": return self.respond(503, b"must-not-retry", [("Retry-After", "1")])
        if mode == "truncated": return self.respond(200, CONTENT[:1024], length=len(CONTENT))
        if mode == "timeout": return self.respond(200, CONTENT[:1024], length=len(CONTENT), stall=True)
        if mode in ("auth-refresh", "auth-fail") and "Authorization" not in self.headers:
            return self.respond(401, b"must-not-write", [("WWW-Authenticate", 'Basic realm="m1"')])
        if mode == "redirect-loop": return self.respond(302, b"must-not-write", [("Location", "/m1/redirect-loop#ignored")])
        if mode == "redirect-chain":
            step = int(query.get("step", [0])[0])
            return self.respond(302, b"must-not-write", [("Location", f"/m1/redirect-chain?step={step+1}")])
        if mode in ("cross-origin", "downgrade"):
            return self.respond(302, b"must-not-write", [("Location", self.server.peer + "/m1/normal")])
        if mode == "userinfo-redirect":
            target = self.server.peer.replace("://", "://user:synthetic-secret@") + "/m1/normal"
            return self.respond(302, b"must-not-write", [("Location", target)])
        if mode == "same-origin": return self.respond(302, b"must-not-write", [("Location", "./normal")])
        resumes = mode.startswith("resume-") or mode in ("weak-etag", "no-etag", "last-modified", "cancel")
        if resumes and count == 1:
            if mode == "resume-complete-416": return self.respond(200, CONTENT, chunked=True, stall=True)
            if mode == "resume-unknown": return self.respond(200, CONTENT[:1024], chunked=True, stall=True)
            etag = 'W/"m1-v1"' if mode == "weak-etag" else None if mode in ("no-etag", "last-modified") else ETAG
            extra = [("Last-Modified", "Wed, 21 Oct 2015 07:28:00 GMT")] if mode == "last-modified" else []
            return self.respond(200, CONTENT[:1024], extra, etag=etag, length=len(CONTENT), stall=mode != "resume-on-error")
        range_header = self.headers.get("Range")
        if range_header:
            start = int(range_header.removeprefix("bytes=").removesuffix("-"))
            if mode in ("resume-416", "resume-complete-416") or start >= len(CONTENT):
                return self.respond(416, b"must-not-write", [("Content-Range", f"bytes */{len(CONTENT)}")])
            if mode == "resume-auth" and "Authorization" not in self.headers:
                return self.respond(401, b"must-not-write", [("WWW-Authenticate", 'Basic realm="m1"')])
            if mode == "resume-ignore": return self.respond(200, CHANGED, etag='"m1-v2"')
            last, total, body, etag = len(CONTENT) - 1, len(CONTENT), CONTENT[start:], ETAG
            if mode == "resume-partial": last = min(start + 65535, last); body = CONTENT[start:last+1]
            if mode == "resume-changed": etag = '"m1-v2"'; body = CHANGED[start:]
            if mode == "resume-length-changed": total += 4; last += 4; body = (CHANGED + b"more")[start:]
            content_range = f"bytes {start}-{last}/{total}"
            if mode in ("resume-bad-start", "resume-invalid-after-restart"): content_range = f"bytes {start+1}-{last}/{total}"
            if mode == "resume-bad-end": content_range = f"bytes {start}-0/{total}"
            if mode == "resume-bad-total": content_range = f"bytes {start}-{last}/1"
            if mode == "resume-overflow": content_range = f"bytes {start}-{2**63}/{2**63+1}"
            extra = [] if mode == "resume-missing-range" else [("Content-Range", content_range)]
            if mode == "resume-missing-etag": etag = None
            if mode == "resume-weak206": etag = 'W/"m1-v1"'
            if mode == "resume-short206": return self.respond(206, body[:100], extra, etag=etag, length=len(body))
            if mode == "resume-extra206": return self.respond(206, CONTENT[start:start+2048],
                [("Content-Range", f"bytes {start}-{start+1023}/{total}")], chunked=True)
            return self.respond(206, body, extra, etag=etag)
        if mode == "resume-invalid-after-restart" and count > 1:
            return self.respond(206, CONTENT, [("Content-Range", f"bytes 0-{len(CONTENT)-1}/{len(CONTENT)}")])
        if mode == "resume-changed": return self.respond(200, CHANGED, etag='"m1-v2"')
        if mode == "resume-length-changed": return self.respond(200, CHANGED + b"more")
        return self.respond(200, CONTENT)
