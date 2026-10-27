"""Android lifecycle barriers layered on the unchanged shared M1/M3 scenarios."""
import time
from http_fixture import CONTENT
from m1_fixture import ETAG, LARGE_BLOCK, LARGE_SIZE
from m3_fixture import M3Handler


class M4Handler(M3Handler):
    def handle_get(self, path, query):
        if not path.startswith("/m4/"):
            return super().handle_get(path, query)
        if path == "/m4/nonascii-etag":
            return self.respond(200, CONTENT, etag='"\x80"')
        if path == "/m4/request-unicode":
            actual = self.headers.get("X-Text", "").encode("latin-1")
            return self.respond(200 if actual == "中文".encode("utf-8") else 400, CONTENT)
        if path == "/m4/slow-headers":
            self.wfile.write(b"HTTP/1.1 200 OK\r\nX-Slow: ")
            self.wfile.flush()
            for _ in range(60):
                self.wfile.write(b"x")
                self.wfile.flush()
                time.sleep(0.05)
            self.wfile.write(b"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            self.wfile.flush()
            self.close_connection = True
            return
        with self.server.requests_lock:
            counts = getattr(self.server, "m4_counts", {})
            counts[path] = counts.get(path, 0) + 1
            self.server.m4_counts = counts
        range_header = self.headers.get("Range")
        if path == "/m4/large":
            start = int(range_header.removeprefix("bytes=").removesuffix("-")) if range_header else 0
            first = not range_header and counts[path] == 1
            self.send_response(206 if range_header else 200)
            self.send_header("Content-Length", str(LARGE_SIZE - start))
            self.send_header("ETag", ETAG)
            self.send_header("Connection", "close")
            if range_header:
                self.send_header("Content-Range", f"bytes {start}-{LARGE_SIZE-1}/{LARGE_SIZE}")
            self.end_headers()
            end = 2**32 if first else LARGE_SIZE
            offset = start
            while offset < end and not self.server.stopping.is_set():
                block = LARGE_BLOCK[offset % 65536:][:min(end - offset, 65536 - offset % 65536)]
                self.wfile.write(block)
                offset += len(block)
            if first:
                time.sleep(2)  # The following data crosses both 4 GiB and the time checkpoint threshold.
                self.wfile.write(LARGE_BLOCK[:32768])
                self.wfile.flush()
                self.wait_connected(120000)
            self.close_connection = True
            return
        if range_header:
            start = int(range_header.removeprefix("bytes=").removesuffix("-"))
            return self.respond(206, CONTENT[start:], [("Content-Range", f"bytes {start}-{len(CONTENT)-1}/{len(CONTENT)}")], etag=ETAG)
        self.send_response(200)
        self.send_header("Content-Length", str(len(CONTENT)))
        self.send_header("ETag", ETAG)
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(CONTENT[:1024])
        self.wfile.flush()
        self.wait_connected(120000)
        self.close_connection = True
