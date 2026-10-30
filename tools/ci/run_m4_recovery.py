"""Kill/force-stop only the instrumented sample process, then verify real durable resume."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/server"))
from http_fixture import RunningFixture
from m4_fixture import M4Handler
from android_reverse import allocate_reverse

PACKAGE = "com.luofelix.fetchkit"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--adb", default=shutil.which("adb"))
    parser.add_argument("--device-host", help="Explicit host alias; omitted uses a private adb reverse rule")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--reboot", action="store_true", help="Also reboot the selected emulator guest at the durable checkpoint")
    parser.add_argument("--reboot-only", action="store_true", help="Run only emulator guest reboot recovery")
    parser.add_argument("--large-file", action="store_true", help="Actually transfer >4 GiB; requires at least 4.8 GiB free on device")
    parser.add_argument("--output", type=Path, default=ROOT / "build/m4-device-recovery")
    args = parser.parse_args()
    if not args.adb:
        parser.error("adb is required")
    if (args.reboot or args.reboot_only) and not args.serial.startswith("emulator-"):
        parser.error("Automated reboot is limited to emulator guests; physical-device reboot needs a separate operator procedure")
    args.output.mkdir(parents=True, exist_ok=True)
    adb = [args.adb, "-s", args.serial]
    if not args.skip_build:
        command = (["cmd.exe", "/d", "/c", str(ROOT / "gradlew.bat")] if sys.platform == "win32" else ["sh", str(ROOT / "gradlew")])
        with (args.output / "build.log").open("w", encoding="utf-8") as log:
            subprocess.run(command + [":sample:assembleDebug", ":sample:assembleDebugAndroidTest", "-Pfetchkit.probes=false", "--max-workers=2"],
                cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=900)
    subprocess.run(adb + ["wait-for-device"], check=True, timeout=120, capture_output=True)
    apk_fingerprints = {}
    for path in [ROOT / "platform/android/sample/build/outputs/apk/debug/sample-debug.apk",
                 ROOT / "platform/android/sample/build/outputs/apk/androidTest/debug/sample-debug-androidTest.apk"]:
        result = subprocess.run(adb + ["install", "--no-streaming", "-r", str(path)], timeout=120, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        apk_fingerprints[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()

    def read(path, private=False):
        command = ["exec-out"] + (["run-as", PACKAGE] if private else []) + ["cat", path]
        try:
            result = subprocess.run(adb + command, capture_output=True, text=True, timeout=15)
            return result.stdout if result.returncode == 0 else ""
        except subprocess.TimeoutExpired:
            return ""

    def start(kind, phase, run_id, url):
        device_log = f"/data/local/tmp/fetchkit-m4-{run_id}-{phase}.txt"
        arguments = ["am", "instrument", "-w", "-e", "class", "com.luofelix.fetchkit.sample.ProcessRecoveryTest",
            "-e", "fetchkitM4RunId", run_id, "-e", "fetchkitM4Url", url, "-e", "fetchkitM4DeathKind", kind,
            "-e", "fetchkitM4Phase", phase, PACKAGE + ".test/androidx.test.runner.AndroidJUnitRunner"]
        command = " ".join(shlex.quote(value) for value in arguments) + " > " + shlex.quote(device_log) + " 2>&1"
        log = (args.output / f"{kind}-{phase}-launcher.log").open("w", encoding="utf-8")
        child = subprocess.Popen(adb + ["shell", "nohup sh -c " + shlex.quote(command) + " >/dev/null 2>&1 </dev/null &"],
            stdout=log, stderr=subprocess.STDOUT)
        return child, log, device_log

    def finish(child, stream, device_log, kind, phase):
        text = read(device_log)
        (args.output / f"{kind}-{phase}.log").write_text(text, encoding="utf-8")
        if child.poll() is None:
            child.terminate()
        child.wait(timeout=15)
        stream.close()
        try:
            subprocess.run(adb + ["shell", "rm", "-f", device_log], timeout=15, capture_output=True)
        except subprocess.TimeoutExpired:
            pass
        return text

    report = {"actual_large_file_executed": args.large_file, "reboot_tested": False, "power_loss_tested": False,
              "force_stop_scope": "Explicit instrumentation relaunch only; no automatic restart guarantee", "cases": [], "passed": False,
              "api": subprocess.check_output(adb + ["shell", "getprop", "ro.build.version.sdk"], text=True).strip(),
              "abis": subprocess.check_output(adb + ["shell", "getprop", "ro.product.cpu.abilist"], text=True).strip(),
              "apk_sha256": apk_fingerprints}
    rule = None
    try:
        with RunningFixture(handler=M4Handler) as fixture:
            if args.device_host:
                url = f"http://{args.device_host}:{fixture.server.server_port}"
            else:
                port, rule = allocate_reverse(adb, fixture.server.server_port)
                url = f"http://127.0.0.1:{port}"
            kinds = (["reboot"] if args.reboot_only else ["kill", "force-stop"] + (["reboot"] if args.reboot else []))
            for kind in kinds + (["large"] if args.large_file else []):
                run_id = str(uuid.uuid4())
                child, stream, device_log = start(kind, "prepare", run_id, url)
                try:
                    deadline = time.monotonic() + (3600 if kind == "large" else 90)
                    marker = None
                    while time.monotonic() < deadline:
                        raw = read(f"files/m4-process-{kind}-ready.json", private=True)
                        try:
                            candidate = json.loads(raw)
                            if candidate.get("run_id") == run_id:
                                marker = candidate
                                break
                        except ValueError:
                            pass
                        log = read(device_log)
                        if "FAILURES!!!" in log or "Process crashed" in log:
                            raise RuntimeError(kind + " prepare failed")
                        time.sleep(0.1)
                    if marker is None:
                        raise RuntimeError(kind + " checkpoint deadline")
                    pid = str(marker["pid"])
                    pids = subprocess.check_output(adb + ["shell", "pidof", PACKAGE], text=True).split()
                    if not pid.isdecimal() or pid not in pids:
                        raise RuntimeError("Prepared PID is not the sample process")
                    before_boot = read("/proc/sys/kernel/random/boot_id").strip() if kind == "reboot" else None
                    command = (["reboot"] if kind == "reboot" else ["shell", "am", "force-stop", PACKAGE] if kind == "force-stop" else
                               ["shell", "run-as", PACKAGE, "touch", f"files/m4-process-{kind}-{run_id}-signal"])
                    killed = subprocess.run(adb + command, timeout=20, capture_output=True, text=True)
                    if killed.returncode:
                        raise RuntimeError("Owned sample termination failed: " + killed.stdout + killed.stderr)
                    if kind == "reboot":
                        deadline = time.monotonic() + 300
                        while time.monotonic() < deadline:
                            after_boot = read("/proc/sys/kernel/random/boot_id").strip()
                            boot = subprocess.run(adb + ["shell", "getprop", "sys.boot_completed"], capture_output=True, text=True, timeout=15)
                            if before_boot and after_boot and before_boot != after_boot and boot.stdout.strip() == "1":
                                break
                            time.sleep(1)
                        else:
                            raise RuntimeError("Emulator reboot/boot-ID deadline")
                        report["boot_id_before"] = before_boot
                        report["boot_id_after"] = after_boot
                        report["reboot_tested"] = True
                        report["reboot_scope"] = "Orderly adb reboot of an emulator guest; no physical-device or power-loss claim"
                        if rule is not None:
                            subprocess.run(adb + ["reverse", "--remove", rule], timeout=15, capture_output=True)
                            port, rule = allocate_reverse(adb, fixture.server.server_port)
                            url = f"http://127.0.0.1:{port}"
                    else:
                        for _ in range(50):
                            remaining = subprocess.run(adb + ["shell", "pidof", PACKAGE], capture_output=True, text=True, timeout=15)
                            if pid not in remaining.stdout.split():
                                break
                            time.sleep(0.1)
                        else:
                            raise RuntimeError("Sample process survived termination")
                finally:
                    finish(child, stream, device_log, kind, "prepare")
                child, stream, device_log = start(kind, "restore", run_id, url)
                try:
                    deadline = time.monotonic() + (10800 if kind == "large" else 180)
                    while time.monotonic() < deadline:
                        text = read(device_log)
                        if any(value in text for value in ("OK (1 test)", "FAILURES!!!", "Process crashed")):
                            break
                        time.sleep(0.5)
                    else:
                        raise RuntimeError(kind + " restore deadline")
                    result = json.loads(read(f"files/m4-process-{kind}-result.json", private=True))
                    if not result.get("passed") or result.get("run_id") != run_id or "OK (1 test)" not in text:
                        raise RuntimeError(kind + " restore assertions failed")
                    path = "/m4/large" if kind == "large" else "/m4/process-" + kind
                    requests = [request for request in fixture.server.requests if request["path"] == path]
                    expected_range = f"bytes={marker['committed_offset']}-"
                    if len(requests) != 2 or requests[1]["range"] != expected_range or requests[1]["if_range"] != '"m1-v1"':
                        raise RuntimeError("Missing exact durable Range/If-Range")
                    if kind == "large" and (result["bytes"] <= 2**32 or marker["committed_offset"] <= 2**32):
                        raise RuntimeError("Large-file test did not cross 4 GiB")
                    result["terminated_pid"] = marker["pid"]
                    result["termination"] = ("emulator-guest-adb-reboot" if kind == "reboot" else "am-force-stop" if kind == "force-stop"
                                             else "controller-triggered-Process.killProcess-SIGKILL")
                    result["range"] = expected_range
                    result["requests"] = requests
                    report["cases"].append(result)
                finally:
                    finish(child, stream, device_log, kind, "restore")
            report["passed"] = True
    finally:
        if rule is not None:
            subprocess.run(adb + ["reverse", "--remove", rule], timeout=15, capture_output=True)
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "cases": len(report["cases"]), "actual_large_file_executed": args.large_file}))


if __name__ == "__main__":
    main()
