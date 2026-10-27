"""M1 real-backend acceptance. All temporary trees are verified before cleanup."""

import argparse
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import platform
import queue
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "server"))
from http_fixture import CONTENT, RunningFixture
from m1_fixture import CHANGED, ETAG, LARGE_BLOCK, LARGE_SIZE, M1Handler


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            value.update(block)
    return value.hexdigest()


def shell_case(cli, fixture, root):
    process = subprocess.Popen([str(cli), "shell", "--root", str(root), "--allow-http"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    lines = queue.Queue()
    def read_lines():
        for line in process.stdout:
            lines.put(line)
    reader = threading.Thread(target=read_lines, daemon=True)
    reader.start()
    def command(value):
        process.stdin.write(value + "\n")
        process.stdin.flush()
        return json.loads(lines.get(timeout=5))
    def await_state(task, state):
        end = time.monotonic() + 5
        while time.monotonic() < end:
            snapshot = command(f"query {task}")
            if snapshot.get("state") == state:
                return snapshot
            time.sleep(0.005)
        raise AssertionError("CLI state deadline")
    try:
        task = command(f'enqueue "shell" "{fixture.url}/m1/resume-shell"')["accepted"]
        other = command(f'enqueue "parallel" "{fixture.url}/m1/normal"')["accepted"]
        check(await_state(other, 9)["published"], "M3 second task must make progress beside a stalled task")
        end = time.monotonic() + 5
        while time.monotonic() < end:
            if command(f"query {task}")["received_bytes"] >= 1024:
                break
            time.sleep(0.005)
        check(command(f"pause {task}")["status"] == "Ok", "CLI pause")
        paused = await_state(task, 2)
        second = subprocess.run([str(cli), "download", fixture.url + "/m1/normal", "--root", str(root), "--allow-http"],
                                capture_output=True, text=True, timeout=5)
        check(second.returncode == 8 and "Busy" in second.stderr, "second process directory lock")
        check(command(f"resume {task}")["status"] == "Ok", "CLI resume")
        done = await_state(task, 9)
        check((root / "objects" / (done["persistent_id"] + ".obj")).read_bytes() == CONTENT, "CLI shell published bytes")
        check(command(f"release {task}")["status"] == "Ok", "CLI release")
        check(command(f"query {task}").get("error") == "Closed", "CLI released task")
        process.stdin.write("quit\n"); process.stdin.flush()
        process.wait(timeout=5)
        return {"id": "cli-shell-and-process-lock", "passed": True, "paused_bytes": paused["received_bytes"]}
    finally:
        if process.poll() is None:
            process.kill(); process.wait(timeout=5)
        reader.join(timeout=5)
        process.stdin.close(); process.stdout.close(); process.stderr.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--large-file", action="store_true")
    parser.add_argument("--openssl", default=shutil.which("openssl"))
    args = parser.parse_args()
    args.driver, args.cli = args.driver.resolve(), args.cli.resolve()
    args.work_root = args.work_root.resolve()
    args.work_root.mkdir(parents=True, exist_ok=True)
    report = {"platform": platform.platform(), "large_file": args.large_file, "cases": [], "failures": []}
    temporary = tempfile.TemporaryDirectory(prefix="m1-", dir=args.work_root)
    workspace = Path(temporary.name).resolve()
    check(workspace.parent == args.work_root, "temporary directory boundary")
    try:
        with ExitStack() as stack:
            plain = stack.enter_context(RunningFixture(handler=M1Handler))
            peer = stack.enter_context(RunningFixture(handler=M1Handler))
            plain.server.peer = peer.url
            peer.server.peer = plain.url
            def execute(mode, fixture=plain, ca="", expected=CONTENT, error=None, state=None):
                case_root = workspace / mode
                item = {"id": mode, "passed": False}
                report["cases"].append(item)
                with fixture.server.requests_lock:
                    fixture.server.mode_counts = {}
                    fixture.server.requests.clear()
                with peer.server.requests_lock:
                    peer.server.requests.clear()
                started = time.monotonic()
                result = subprocess.run([str(args.driver), fixture.url, mode, str(case_root), str(ca), peer.url],
                                        capture_output=True, text=True, timeout=600 if args.large_file else 20)
                item.update({"driver_exit": result.returncode, "seconds": round(time.monotonic()-started, 3)})
                check(result.returncode == 0, f"{mode}: driver failed ({result.returncode})")
                check("synthetic-secret" not in result.stdout + result.stderr, f"{mode}: credential leaked")
                data = json.loads(result.stdout)
                snapshot = data["snapshot"]
                item.update({name: snapshot[name] for name in ["state", "error", "received_bytes", "total_known", "total_bytes",
                    "http_status", "platform_code", "generation", "request_count"]})
                item.update({"bytes": snapshot["received_bytes"], "paused_bytes": data["paused_bytes"],
                    "requests": list(fixture.server.requests), "refreshes": data["refreshes"]})
                check(snapshot["published"] == (snapshot["state"] == 9), f"{mode}: inconsistent publication")
                check(data["closed_resume"] == "Closed" and data["release"] == "Ok" and data["stale_query"] == "Closed", f"{mode}: handle lifecycle")
                check(data["reentry_ok"], f"{mode}: provider reentry/self-close barrier")
                if error:
                    check(snapshot["state"] == (5 if state is None and error == "Cancelled" else 4 if state is None else state)
                          and snapshot["error"] == error, f"{mode}: wrong failure {snapshot['state']}/{snapshot['error']}")
                else:
                    check(snapshot["state"] == 9 and snapshot["error"] == "Ok" and snapshot["integrity"] == 1,
                          f"{mode}: unexpected {snapshot['state']}/{snapshot['error']}")
                staged = (case_root / "objects" / (snapshot["persistent_id"] + ".obj")).resolve() if snapshot["published"] else Path(snapshot["staging_path"]).resolve()
                check(staged.parent == case_root.resolve() / ("objects" if snapshot["published"] else "tmp"), f"{mode}: storage boundary")
                if expected is not None and not error:
                    check(staged.read_bytes() == expected, f"{mode}: mixed/wrong bytes")
                    check(snapshot["received_bytes"] == len(expected), f"{mode}: wrong 64-bit offset")
                if error == "Cancelled":
                    check(not staged.exists(), f"{mode}: cancelled data not removed")
                requests = list(fixture.server.requests)
                check(all(r["method"] == "GET" for r in requests), f"{mode}: HEAD dependency")
                check(all(r["accept_encoding"] == "identity" for r in requests), f"{mode}: identity missing")
                if data["paused_bytes"]:
                    check(snapshot["generation"] == 2, f"{mode}: generation not advanced")
                    if mode in ("weak-etag", "no-etag", "last-modified"):
                        check(requests[1]["range"] is None, f"{mode}: untrusted continuation")
                    else:
                        check(requests[1]["range"] == f'bytes={data["paused_bytes"]}-' and requests[1]["if_range"] == ETAG,
                              f"{mode}: wrong Range/If-Range")
                item["passed"] = True
                print(f'{mode}: {snapshot["error"]}, {snapshot["received_bytes"]} bytes', flush=True)
                return data, requests, staged

            if args.large_file:
                data, requests, staged = execute("large", expected=None)
                check(data["snapshot"]["received_bytes"] == LARGE_SIZE and staged.stat().st_size == LARGE_SIZE, "actual >4 GiB length")
                expected_hash = hashlib.sha256()
                remaining = LARGE_SIZE
                while remaining:
                    block = LARGE_BLOCK[:min(len(LARGE_BLOCK), remaining)]
                    expected_hash.update(block); remaining -= len(block)
                actual_hash = digest(staged)
                check(actual_hash == expected_hash.hexdigest(), "actual >4 GiB content")
                report["large_file_bytes"] = LARGE_SIZE
                report["sha256"] = actual_hash
            else:
                for mode, content in [("normal", CONTENT), ("empty", b""), ("unknown", CONTENT), ("unknown-close", CONTENT), ("no-head", CONTENT),
                    ("resume-normal", CONTENT), ("resume-ignore", CHANGED), ("resume-partial", CONTENT),
                    ("resume-416", CONTENT), ("resume-complete-416", CONTENT), ("resume-changed", CHANGED),
                    ("resume-length-changed", CHANGED+b"more"), ("resume-bad-start", CONTENT), ("resume-bad-end", CONTENT),
                    ("resume-bad-total", CONTENT), ("resume-overflow", CONTENT), ("resume-missing-range", CONTENT),
                    ("resume-missing-etag", CONTENT), ("resume-weak206", CONTENT), ("weak-etag", CONTENT),
                    ("no-etag", CONTENT), ("last-modified", CONTENT), ("resume-on-error", CONTENT), ("resume-auth", CONTENT), ("resume-unknown", CONTENT)]:
                    execute(mode, expected=content)
                for mode, error in [("resume-short206", "Network"), ("resume-extra206", "ProtocolMismatch"),
                    ("resume-invalid-after-restart", "ProtocolMismatch"), ("gzip", "ProtocolMismatch"),
                    ("conflicting-length", "ProtocolMismatch"), ("overflow-length", "ProtocolMismatch"),
                    ("length-mismatch", "IntegrityMismatch"), ("not-found", "HttpStatus"), ("unavailable", "HttpStatus"),
                    ("fresh-416", "ProtocolMismatch"),
                    ("truncated", "Network"), ("timeout", "Timeout"), ("unknown-limit", "QuotaExceeded"),
                    ("redirect-loop", "ProtocolMismatch"), ("redirect-chain", "ProtocolMismatch"),
                    ("userinfo-redirect", "ProtocolMismatch"), ("auth-fail", "AuthRequired"), ("provider-throws", "AuthRequired"),
                    ("cancel", "Cancelled"), ("provider-cancel", "Cancelled")]:
                    data, requests, staged = execute(mode, expected=None, error=error)
                    if mode == "unavailable": check(len(requests) == 1, "hidden status retry")
                    if mode == "auth-fail": check(data["refreshes"] == 1 and len(requests) == 2, "unbounded auth refresh")
                    if mode == "resume-invalid-after-restart": check(len(requests) == 3, "unbounded safe restart")
                data, requests, _ = execute("auth-refresh")
                check(data["refreshes"] == 1 and len(requests) == 2, "host authentication entry")
                for mode in ["cross-origin", "same-origin", "provider-cross-origin"]:
                    execute(mode)
                    target = peer.server.requests if mode != "same-origin" else plain.server.requests[-1:]
                    check(target, "redirect/provider target missing")
                    for request in target:
                        check(all(request[name] == (mode == "same-origin") for name in ["has_authorization", "has_cookie", "has_api_key"]),
                              "origin credential separation")
                check(args.openssl is not None, "OpenSSL executable required for local HTTPS verification")
                key, cert = workspace/"server-key.pem", workspace/"server-cert.pem"
                generated = subprocess.run([args.openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-keyout", str(key), "-out", str(cert), "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost"],
                    capture_output=True, timeout=30)
                check(generated.returncode == 0, "test certificate generation")
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); context.load_cert_chain(cert, key)
                secure = stack.enter_context(RunningFixture(handler=M1Handler, tls_context=context))
                secure.server.peer = peer.url
                execute("tls-normal", secure, cert)
                execute("untrusted-tls", secure, expected=None, error="CertificateError")
                execute("downgrade", secure, cert, expected=None, error="ProtocolMismatch")
                check(not peer.server.requests, "HTTPS downgrade reached cleartext origin")
                report["cases"].append(shell_case(args.cli, plain, workspace/"cli-shell"))
                for mode, options, expected_exit in [("normal", ["--expected-length", str(len(CONTENT))], 0),
                    ("not-found", [], 4), ("gzip", [], 5)]:
                    run = subprocess.run([str(args.cli), "download", plain.url+"/m1/"+mode,
                        "--root", str(workspace/("cli-"+mode)), "--allow-http"]+options, capture_output=True, text=True, timeout=10)
                    check(run.returncode == expected_exit, "CLI exit code")
                report["cases"].append({"id": "cli-download-exit-codes", "passed": True})
                denied = subprocess.run([str(args.cli), "download", plain.url+"/m1/normal", "--root", str(workspace/"http-denied")],
                                        capture_output=True, text=True, timeout=5)
                check(denied.returncode == 2, "HTTP was not explicit")
                report["cases"].append({"id": "http-explicit-permission", "passed": True})
    except Exception as error:
        report["failures"].append(str(error))
        print(f"FAIL: {error}", file=sys.stderr)
    finally:
        # Windows recursive cleanup is confined to this verified test workspace.
        check(workspace.resolve().parent == args.work_root, "cleanup boundary")
        temporary.cleanup()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8", newline="\n")
    return 1 if report["failures"] else 0


if __name__ == "__main__":
    sys.exit(main())
