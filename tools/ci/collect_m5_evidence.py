"""Archive completed local M5 evidence; binaries remain under build/."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def copy_text(source, destination):
    # Git stores repository text as LF. Hash the same bytes a fresh checkout will contain.
    destination.write_text(source.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work-root", type=Path, default=ROOT / "build")
    parser.add_argument("--output", type=Path, default=ROOT / "docs/reports/m5")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    reports = {
        "native-consumer.json": "m5-consumer/report.json",
        "android-consumer.json": "m5-aar-consumer/report.json",
        "android-matrix.json": "m5-android/report.json",
        "device-recovery.json": "m5-device-recovery/report.json",
        "stress.json": "m5-stress/report.json",
        "size-arm64.json": "m5-size-arm64/report.json",
        "package-manifest.json": "m5-preview/manifest.json",
        "dependency-licenses.json": "m5-preview/dependency-licenses.json",
        "host-protocol.json": "m3-pinned/reports/m1-protocol.json",
        "host-scheduler.json": "m3-pinned/reports/m3-protocol.json",
        "host-recovery.json": "m3-pinned/reports/m2-recovery.json",
        "host-curl-contract.json": "m3-pinned/reports/curl-contract.json",
    }
    for name, relative in reports.items():
        path = args.work_root / relative
        data = json.loads(path.read_text(encoding="utf-8"))
        if name in ("native-consumer.json", "android-consumer.json", "android-matrix.json", "device-recovery.json", "stress.json"):
            if data.get("passed") is not True:
                raise RuntimeError("Incomplete evidence: " + name)
        if name.startswith("host-") and (data.get("failures") or not all(item.get("passed", True) for item in data.get("cases", []))):
            raise RuntimeError("Host report failure: " + name)
        copy_text(path, args.output / name)
    host_log = args.work_root / "m5-host-ctest.log"
    if "100% tests passed, 0 tests failed out of 10" not in host_log.read_text(encoding="utf-8"):
        raise RuntimeError("Host CTest acceptance incomplete")
    copy_text(host_log, args.output / "host-ctest.txt")
    copy_text(args.work_root / "m5-size-arm64/sdk-dependencies.txt", args.output / "android-dependencies.txt")
    copy_text(args.work_root / "m5-publish.log", args.output / "maven-publish.txt")
    identity = {"base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "working_tree_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
                "scope": "Windows host and API31 x86_64 emulator; not Linux or physical-device acceptance",
                "evidence": {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in args.output.iterdir()
                             if path.is_file() and path.name not in ("evidence-index.json", "release-readiness.json")}}
    (args.output / "evidence-index.json").write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
