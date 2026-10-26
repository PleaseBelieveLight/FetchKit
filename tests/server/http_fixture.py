"""Deterministic M0 HTTP fixture. Stdlib only; never logs request credentials."""

import argparse
import gzip
import json
from pathlib import Path
import select
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

SCENARIOS = Path(__file__).resolve().parents[1] / "fixtures/m0_scenarios.json"
CONTENT = bytes(i % 251 for i in range(262144))
ETAG = '"fetchkit-m0-v1"'


class FixtureServer(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, address=("127.0.0.1", 0), handler=None):
        super().__init__(address, handler or FixtureHandler)
        self.stopping = threading.Event()
        self.requests = []
        self.requests_lock = threading.Lock()

    def server_close(self):
        self.stopping.set()
        super().server_close()


class FixtureHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def wait_connected(self, milliseconds):
        deadline = time.monotonic() + milliseconds / 1000
        while not self.server.stopping.is_set():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return True
            readable, _, _ = select.select([self.connection], [], [], min(remaining, 0.05))
            if readable and not self.connection.recv(1, socket.MSG_PEEK):
                return False
        return False

    def send_body(self, status, body, extra=(), declared_length=None):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body) if declared_length is None else declared_length))
        self.send_header("ETag", ETAG)
        self.send_header("Connection", "close")
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        self.close_connection = True

    def do_GET(self):
        parsed = urlsplit(self.path)
        query = parse_qs(parsed.query)
        # Only protocol-relevant, non-secret fields are retained. No full URL/query.
        with self.server.requests_lock:
            self.server.requests.append({
                "path": parsed.path,
                "method": "GET",
                "range": self.headers.get("Range"),
                "if_range": self.headers.get("If-Range"),
                "accept_encoding": self.headers.get("Accept-Encoding"),
                "has_authorization": "Authorization" in self.headers,
                "has_cookie": "Cookie" in self.headers,
                "has_api_key": "X-Api-Key" in self.headers,
            })
        try:
            self.handle_get(parsed.path, query)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
            self.close_connection = True

    def handle_get(self, path, query):
        if path == "/scenarios":
            return self.send_body(200, SCENARIOS.read_bytes(), [("Content-Type", "application/json")])
        if path == "/echo":
            body = json.dumps({
                "has_authorization": "Authorization" in self.headers,
                "has_cookie": "Cookie" in self.headers,
            }).encode()
            return self.send_body(200, body)
        if path == "/auth":
            if "Authorization" in self.headers:
                return self.send_body(200, b"")
            return self.send_body(401, b"", [("WWW-Authenticate", 'Basic realm="fetchkit-m0"')])
        if path in ("/redirect", "/redirect-loop"):
            target = "/bytes" if path == "/redirect" else "/redirect-loop"
            return self.send_body(302, b"", [("Location", target)])
        if path == "/delay-headers":
            if not self.wait_connected(min(int(query.get("ms", [5000])[0]), 10000)):
                return
        if path == "/stall-body":
            self.send_response(200)
            self.send_header("Content-Length", str(len(CONTENT)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(CONTENT[:1024])
            self.wfile.flush()
            if self.wait_connected(min(int(query.get("ms", [5000])[0]), 10000)):
                self.wfile.write(CONTENT[1024:])
            self.close_connection = True
            return
        if path == "/gzip":
            return self.send_body(200, gzip.compress(CONTENT, mtime=0), [("Content-Encoding", "gzip")])
        if path == "/headers":
            return self.send_body(200, CONTENT, [("X-FetchKit", "one"), ("X-FetchKit", "two")])
        if path == "/truncated":
            return self.send_body(200, CONTENT[:1024], declared_length=len(CONTENT))
        if path == "/range":
            value = self.headers.get("Range", "")
            if value:
                try:
                    if not value.startswith("bytes=") or not value.endswith("-"):
                        raise ValueError()
                    start = int(value[6:-1])
                    if start < 0 or start >= len(CONTENT):
                        raise ValueError()
                except ValueError:
                    return self.send_body(416, b"", [("Content-Range", f"bytes */{len(CONTENT)}")])
                return self.send_body(206, CONTENT[start:], [
                    ("Content-Range", f"bytes {start}-{len(CONTENT)-1}/{len(CONTENT)}")])
        if path in ("/bytes", "/range", "/delay-headers"):
            return self.send_body(200, CONTENT)
        return self.send_body(404, b"")


class RunningFixture:
    def __init__(self, host="127.0.0.1", port=0, handler=None, tls_context=None):
        self.server = FixtureServer((host, port), handler)
        self.scheme = "https" if tls_context else "http"
        if tls_context:
            self.server.socket = tls_context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def url(self):
        return f"{self.scheme}://127.0.0.1:{self.server.server_port}"

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_args):
        self.server.stopping.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--ready-file", type=Path)
    args = parser.parse_args()
    with RunningFixture(args.host, args.port) as fixture:
        ready = json.dumps({"port": fixture.server.server_port, "url": fixture.url})
        if args.ready_file:
            args.ready_file.parent.mkdir(parents=True, exist_ok=True)
            args.ready_file.write_text(ready, encoding="utf-8")
        print(ready, flush=True)
        try:
            while True:
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
