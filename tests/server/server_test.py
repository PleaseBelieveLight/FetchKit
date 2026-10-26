import gzip
import http.client
import unittest
from urllib.parse import urlsplit

from http_fixture import CONTENT, RunningFixture


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = RunningFixture().__enter__()

    @classmethod
    def tearDownClass(cls):
        cls.fixture.__exit__()

    def get(self, path, headers=None):
        url = urlsplit(self.fixture.url)
        connection = http.client.HTTPConnection(url.hostname, url.port, timeout=2)
        self.addCleanup(connection.close)
        connection.request("GET", path, headers=headers or {})
        return connection.getresponse()

    def test_content(self):
        response = self.get("/bytes")
        self.assertEqual(response.status, 200)
        self.assertEqual(response.read(), CONTENT)

    def test_range(self):
        response = self.get("/range", {"Range": "bytes=1024-"})
        self.assertEqual(response.status, 206)
        self.assertEqual(response.getheader("Content-Range"), "bytes 1024-262143/262144")
        self.assertEqual(response.read(), CONTENT[1024:])

    def test_unsatisfied_range(self):
        response = self.get("/range", {"Range": "bytes=262144-"})
        self.assertEqual(response.status, 416)
        self.assertEqual(response.getheader("Content-Range"), "bytes */262144")

    def test_duplicate_headers(self):
        response = self.get("/headers")
        self.assertEqual([value for key, value in response.getheaders() if key == "X-FetchKit"], ["one", "two"])

    def test_gzip_even_when_identity_requested(self):
        response = self.get("/gzip", {"Accept-Encoding": "identity"})
        self.assertEqual(gzip.decompress(response.read()), CONTENT)

    def test_redirect_is_explicit(self):
        response = self.get("/redirect")
        self.assertEqual(response.status, 302)
        self.assertEqual(response.getheader("Location"), "/bytes")

    def test_truncation(self):
        response = self.get("/truncated")
        with self.assertRaises(http.client.IncompleteRead):
            response.read()

    def test_secret_values_are_not_recorded(self):
        response = self.get("/echo?token=secret-sentinel", {"Authorization": "secret-sentinel", "Cookie": "secret-sentinel"})
        self.assertIn(b'"has_cookie": true', response.read())
        self.assertNotIn("secret-sentinel", repr(self.fixture.server.requests))


if __name__ == "__main__":
    unittest.main()
