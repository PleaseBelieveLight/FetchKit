"""Build/run opt-in probes on one existing device; remove only our adb reverse rule."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/server"))
from http_fixture import RunningFixture


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--repetitions", type=int, default=100)
    parser.add_argument("--backend", choices=["platform", "okhttp"], default="platform")
    parser.add_argument("--output", type=Path, default=ROOT / "build/m0-android")
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 1000:
        parser.error("repetitions must be 1..1000")
    args.output.mkdir(parents=True, exist_ok=True)
    run_id = str(uuid.uuid4())
    adb = ["adb", "-s", args.serial]
    with RunningFixture() as fixture:
        rule = None
        try:
            properties = ["-Pfetchkit.probes=true", f"-Pfetchkit.probeBackend={args.backend}"]
            command = (["cmd.exe", "/d", "/c", str(ROOT / "gradlew.bat")] if sys.platform == "win32"
                       else ["sh", str(ROOT / "gradlew")])
            with (args.output / "gradle.log").open("w", encoding="utf-8") as log:
                completed = subprocess.run(command + [":sample:assembleDebug", ":sample:assembleDebugAndroidTest"] + properties,
                                           cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=600)
            if completed.returncode:
                raise RuntimeError(f"Probe build failed; see {args.output / 'gradle.log'}")
            subprocess.run(adb + ["wait-for-device"], check=True, timeout=120)
            subprocess.run(adb + ["install", "-r", str(ROOT / "platform/android/sample/build/outputs/apk/debug/sample-debug.apk")], check=True)
            subprocess.run(adb + ["install", "-r", str(ROOT / "platform/android/sample/build/outputs/apk/androidTest/debug/sample-debug-androidTest.apk")], check=True)
            # Allocate on the device after building/installing: adb may restart during
            # a long build, and an existing user's reverse rule must never be replaced.
            device_port = int(subprocess.check_output(adb + ["reverse", "tcp:0", f"tcp:{fixture.server.server_port}"], text=True).strip())
            rule = f"tcp:{device_port}"
            with (args.output / "instrumentation.log").open("w", encoding="utf-8") as log:
                instrument = subprocess.run(adb + ["shell", "am", "instrument", "-w",
                    "-e", "fetchkitProbeUrl", f"http://127.0.0.1:{device_port}",
                    "-e", "fetchkitProbeRepetitions", str(args.repetitions),
                    "-e", "fetchkitProbeRunId", run_id,
                    "com.luofelix.fetchkit.test/androidx.test.runner.AndroidJUnitRunner"],
                    stdout=log, stderr=subprocess.STDOUT, timeout=1800)
            raw = subprocess.check_output(adb + ["exec-out", "run-as", "com.luofelix.fetchkit", "cat", "files/m0-platform.json"])
            report = json.loads(raw)
            if report.get("run_id") != run_id:
                raise RuntimeError("Stale or missing probe result")
            report["instrumentation_exit_code"] = instrument.returncode
            report["fixture_requests"] = len(fixture.server.requests)
            report["identity_request_failures"] = sum(r["accept_encoding"] != "identity" for r in fixture.server.requests)
            if report["identity_request_failures"]:
                report["failures"].append("Some requests did not explicitly select identity encoding")
                report["gate_passed"] = False
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print(json.dumps({key: report.get(key) for key in ["summary", "ambient_cookie_sent", "ambient_auth_calls", "failures", "gate_passed"]}, indent=2))
            return 0 if not instrument.returncode and report["gate_passed"] else 1
        finally:
            if rule is not None:
                cleanup = subprocess.run(adb + ["reverse", "--remove", rule], capture_output=True, text=True)
                if cleanup.returncode:
                    print(f"Could not remove owned adb rule {rule}; device may have disconnected", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
