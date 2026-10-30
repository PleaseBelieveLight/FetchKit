"""Run the production Android SDK against the shared M1/M3 local fixtures."""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import ssl
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/server"))
from http_fixture import RunningFixture
from m4_fixture import M4Handler
from android_reverse import allocate_reverse


def instrumentation_finished(output):
    # 任意用例数的终结输出都应结束等待；随后仍严格校验预期 3 个用例与本次 run_id。
    # 否则缺失用例或 Runner 提前退出会被误当成仍在执行，直至耗尽整个设备超时。
    return bool(re.search(r"\bOK \(\d+ tests?\)", output)) or any(marker in output for marker in
        ("INSTRUMENTATION_CODE:", "FAILURES!!!", "INSTRUMENTATION_FAILED", "Process crashed"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--adb", default=shutil.which("adb"))
    parser.add_argument("--adb-server-port", type=int, default=5037)
    parser.add_argument("--device-host", help="Explicit emulator host alias; omitted uses owned adb reverse rules")
    parser.add_argument("--openssl", default=shutil.which("openssl"))
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--output", type=Path, default=ROOT / "build/m4-android")
    args = parser.parse_args()
    if not args.adb or not args.openssl:
        parser.error("adb and OpenSSL executables are required")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    adb = [args.adb, "-P", str(args.adb_server_port), "-s", args.serial]
    run_id = str(uuid.uuid4())
    device_log = "/data/local/tmp/fetchkit-m4-" + run_id + ".txt"
    cert = ROOT / "tests/fixtures/m4-tls/server-cert.pem"
    key = ROOT / "tests/fixtures/m4-tls/server-key.pem"
    assert cert.read_bytes() == (ROOT / "platform/android/sample/src/debug/res/raw/fetchkit_local_test_cert.pem").read_bytes()
    if not args.skip_build:
        command = (["cmd.exe", "/d", "/c", str(ROOT / "gradlew.bat")] if sys.platform == "win32"
                   else ["sh", str(ROOT / "gradlew")])
        with (args.output / "build.log").open("w", encoding="utf-8") as log:
            subprocess.run(command + [":sdk:testDebugUnitTest", ":sample:assembleDebug", ":sample:assembleDebugAndroidTest",
                ":sample:testDebugUnitTest", "-Pfetchkit.probes=false", "--max-workers=2"],
                cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=900)
    subprocess.run(adb + ["wait-for-device"], check=True, timeout=120, capture_output=True)
    boot_deadline = time.monotonic() + 120
    while time.monotonic() < boot_deadline:
        boot = subprocess.run(adb + ["shell", "getprop", "sys.boot_completed"], capture_output=True, text=True, timeout=15)
        if boot.returncode == 0 and boot.stdout.strip() == "1":
            break
        time.sleep(1)
    else:
        raise RuntimeError("Device boot deadline")
    apk_fingerprints = {}
    for path in [ROOT / "platform/android/sample/build/outputs/apk/debug/sample-debug.apk",
                 ROOT / "platform/android/sample/build/outputs/apk/androidTest/debug/sample-debug-androidTest.apk"]:
        installed = subprocess.run(adb + ["install", "-r", str(path)], timeout=120, capture_output=True, text=True)
        if installed.returncode:
            raise RuntimeError("APK install failed: " + installed.stdout + installed.stderr)
        apk_fingerprints[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    rules = []
    temporary = tempfile.TemporaryDirectory(prefix="tls-", dir=args.output)
    workspace = Path(temporary.name).resolve()
    assert workspace.parent == args.output
    log_text = ""
    try:
        untrusted_key, untrusted_cert = workspace / "key.pem", workspace / "cert.pem"
        subprocess.run([args.openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-keyout", str(untrusted_key), "-out", str(untrusted_cert), "-subj", "/CN=localhost",
            "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost"], check=True, capture_output=True, timeout=30)
        trusted_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        trusted_context.load_cert_chain(cert, key)
        untrusted_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        untrusted_context.load_cert_chain(untrusted_cert, untrusted_key)
        with ExitStack() as stack:
            fixtures = [stack.enter_context(RunningFixture(handler=M4Handler)),
                        stack.enter_context(RunningFixture(handler=M4Handler)),
                        stack.enter_context(RunningFixture(handler=M4Handler, tls_context=trusted_context)),
                        stack.enter_context(RunningFixture(handler=M4Handler, tls_context=untrusted_context))]
            urls = []
            for index, fixture in enumerate(fixtures):
                if args.device_host:
                    port = fixture.server.server_port
                    host = args.device_host
                else:
                    port, rule = allocate_reverse(adb, fixture.server.server_port)
                    rules.append(rule)
                    host = "127.0.0.1"
                urls.append(f"{'https' if index >= 2 else 'http'}://{host}:{port}")
            for fixture in fixtures:
                fixture.server.peer = urls[1]
            instrument = ["am", "instrument", "-w",
                    "-e", "class", "com.luofelix.fetchkit.sample.SdkIntegrationTest,com.luofelix.fetchkit.sample.DownloadIntegrationTest",
                    "-e", "fetchkitM4RunId", run_id, "-e", "fetchkitM4Url", urls[0],
                    "-e", "fetchkitM4Tls", urls[2], "-e", "fetchkitM4UntrustedTls", urls[3],
                    "com.luofelix.fetchkit.test/androidx.test.runner.AndroidJUnitRunner"]
            # Keep the authoritative JUnit output on-device even if the host ADB stream reconnects.
            command = " ".join(shlex.quote(value) for value in instrument) + " > " + shlex.quote(device_log) + " 2>&1"
            launch_log = stack.enter_context((args.output / "launcher.log").open("w", encoding="utf-8"))
            launcher = subprocess.Popen(adb + ["shell", "nohup sh -c " + shlex.quote(command) + " >/dev/null 2>&1 </dev/null &"],
                                        stdout=launch_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 1800
            log_text = ""
            while time.monotonic() < deadline:
                read = subprocess.run(adb + ["exec-out", "cat", device_log], capture_output=True, text=True, timeout=20)
                if read.returncode == 0:
                    log_text = read.stdout
                    if instrumentation_finished(log_text):
                        break
                time.sleep(1)
            else:
                if launcher.poll() is None:
                    launcher.terminate()
                launcher.wait(timeout=15)
                raise RuntimeError("Device JUnit deadline")
            try:
                launcher.wait(timeout=5)
            except subprocess.TimeoutExpired:
                launcher.terminate()
                launcher.wait(timeout=15)
            (args.output / "instrumentation.log").write_text(log_text, encoding="utf-8")
            report = json.loads(subprocess.check_output(adb + ["exec-out", "run-as", "com.luofelix.fetchkit", "cat", "files/m4-android.json"]))
            if report.get("run_id") != run_id:
                raise RuntimeError("Stale device report")
            report["instrumentation_output_on_device"] = True
            report["apk_sha256"] = apk_fingerprints
            report["network_route"] = "explicit-device-host" if args.device_host else "owned-adb-reverse"
            report["junit_passed"] = "OK (3 tests)" in log_text and "FAILURES" not in log_text
            requests = [request for fixture in fixtures for request in fixture.server.requests]
            report["fixture_requests"] = len(requests)
            report["requests"] = requests
            report["identity_request_failures"] = sum(request["accept_encoding"] != "identity" for request in requests)
            report["ambient_cookie_sent"] = any(request["has_cookie"] for request in requests if request["path"] == "/m1/ambient-normal")
            report["peer_credentials_sent"] = any(any(request[name] for name in ("has_authorization", "has_cookie", "has_api_key"))
                for request in fixtures[1].server.requests)
            report["m3_request_counts"] = getattr(fixtures[0].server, "m3_counts", {})
            report["no_hidden_503_retry"] = report["m3_request_counts"].get("android-no-hidden") == 1
            report["fixture_certificate_sha256"] = hashlib.sha256(cert.read_bytes()).hexdigest()
            report["passed"] = (report["passed"] and report["junit_passed"] and
                not report["identity_request_failures"] and not report["ambient_cookie_sent"] and
                not report["peer_credentials_sent"] and report["no_hidden_503_retry"])
            (args.output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            print(json.dumps({key: report[key] for key in ("api", "abi", "failure_count", "fixture_requests", "passed")}, ensure_ascii=False))
            print("Failed cases:", [case["id"] for case in report["cases"] if not case["passed"]])
            return 0 if report["passed"] else 1
    finally:
        # 包括超时和报告缺失在内的失败都保留最近一次实际输出，避免只剩空 launcher 日志。
        (args.output / "instrumentation.log").write_text(log_text, encoding="utf-8")
        try:
            subprocess.run(adb + ["shell", "rm", "-f", device_log], capture_output=True, timeout=15)
        except subprocess.TimeoutExpired:
            print("Could not remove owned device log after ADB timeout", file=sys.stderr)
        for rule in rules:
            subprocess.run(adb + ["reverse", "--remove", rule], capture_output=True, timeout=15)
        assert workspace.resolve().parent == args.output
        temporary.cleanup()


if __name__ == "__main__":
    sys.exit(main())
