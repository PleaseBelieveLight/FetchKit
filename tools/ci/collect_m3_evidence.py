"""Archive M3 Windows evidence; missing platforms remain explicit gates."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-build", type=Path, default=ROOT / "build/m3-pinned")
    parser.add_argument("--without-curl-build", type=Path, default=ROOT / "build/m3-no-curl")
    parser.add_argument("--size-build", type=Path, default=ROOT / "build/m3-size-arm64")
    parser.add_argument("--output", type=Path, default=ROOT / "docs/reports/m3")
    args = parser.parse_args()
    if platform.system() != "Windows":
        parser.error("This collector archives Windows evidence; retain Linux evidence separately.")

    def load(path):
        return json.loads(path.read_text(encoding="utf-8"))

    def read(path):
        return path.read_text(encoding="utf-8", errors="replace")

    def check(condition, message):
        if not condition:
            raise RuntimeError(message)

    protocol = load(args.host_build / "reports/m1-protocol.json")
    recovery = load(args.host_build / "reports/m2-recovery.json")
    m3 = load(args.host_build / "reports/m3-protocol.json")
    contract = load(args.host_build / "reports/curl-contract.json")
    size = load(args.size_build / "report.json")
    for report, count, name in [(protocol, 55, "protocol"), (recovery, 66, "recovery"), (m3, 21, "M3 public API")]:
        check(len(report["cases"]) == count and all(case["passed"] for case in report["cases"]), name)
        check(not report.get("failures") and report.get("passed", True), name + " failures")
    check(not contract["failures"] and len(contract["samples"]) == 508, "curl contract")
    for name, count in [("m3-pinned-ctest.log", 10), ("m3-no-curl-ctest.log", 5)]:
        log = read(ROOT / "build" / name)
        check(f"100% tests passed, 0 tests failed out of {count}" in log, name)
    android = read(ROOT / "build/m3-android.log")
    check("BUILD SUCCESSFUL" in android, "Android build/unit tests")
    scheduler_log = read(args.host_build / "Testing/Temporary/LastTest.log")
    scheduler = re.search(r'Testing: scheduler_storage\n(.*?)<end of output>', scheduler_log, re.S)
    check(scheduler and "21 scheduler/storage suites passed" in scheduler.group(1), "scheduler suites")
    check("cancel-pause-release-close-races: passed" in scheduler.group(1), "20 close race rounds")
    check("engine" in size["targets"] and not size["complete_android_download_cost"], "size scope")
    for name, expected in size["source_sha256"].items():
        check(sha256(ROOT / name) == expected, "size input changed: " + name)

    def profile(build):
        values = {}
        for line in read(build / "CMakeCache.txt").splitlines():
            if line.startswith(("//", "#")) or "=" not in line or ":" not in line.split("=", 1)[0]:
                continue
            key, value = line.split("=", 1)
            key = key.split(":", 1)[0]
            if key.startswith("FETCHKIT_") or key in (
                "CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_DEBUG",
                "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_PREFIX_PATH", "ZLIB_INCLUDE_DIR", "ZLIB_LIBRARY_RELEASE",
                "OPENSSL_INCLUDE_DIR", "OPENSSL_SSL_LIBRARY", "OPENSSL_CRYPTO_LIBRARY",
            ):
                values[key] = value
        return values

    host_profile = profile(args.host_build)
    header = read(Path(host_profile["ZLIB_INCLUDE_DIR"]) / "zlib.h")
    zlib_version = re.search(r'#define\s+ZLIB_VERSION\s+"([^"]+)"', header).group(1)
    artifacts = [args.host_build / "fetchkit_cli.exe", args.host_build / "libfetchkit.a",
                 args.host_build / "fetchkit_scheduler_test.exe",
                 ROOT / "platform/android/sdk/build/outputs/aar/sdk-release.aar"]
    artifacts += [args.size_build / ("stripped/libfetchkit_" + mode + "_measure.so")
                  for mode in ("baseline", "digest", "engine")]
    names = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT
    ).decode().split("\0")
    sources = {}
    for name in names:
        if not name or name.startswith(("docs/reports/", ".idea/")):
            continue
        path = ROOT / name
        if path.is_file() and (path.suffix in (
            ".h", ".hpp", ".cpp", ".c", ".py", ".sh", ".cmake", ".kt", ".kts", ".toml", ".hex", ".map"
        ) or path.name in ("CMakeLists.txt", "gradle.properties", "gradle-wrapper.properties",
                           "curl.lock.json", "picosha2.lock.json", ".gitattributes")):
            sources[name] = sha256(path)
    manifest = {
        "date": "2026-10-27",
        "base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "platform": platform.platform(),
        "linux_executed": False, "sanitizers_executed": False, "reboot_tested": False,
        "power_loss_tested": False, "real_filesystem_enospc_tested": False,
        "device_instrumentation_rerun": False, "actual_large_file_rerun": False,
        "large_file_scope": "M3 tests 64-bit quota arithmetic and real 3x32 MiB transfer. Actual >4 GiB M2 evidence is historical, not an M3 rerun.",
        "source_sha256_scope": "Final Windows pinned, without-curl, Android compilation and arm64 isolated size builds.",
        "validation": {
            "host_ctest": "10/10", "without_curl_ctest": "5/5 (fixture_server covered in host suite)",
            "scheduler_storage_suites": 21, "close_race_rounds": 20,
            "protocol_checks": len(protocol["cases"]), "recovery_checks": len(recovery["cases"]),
            "m3_public_api_checks": len(m3["cases"]), "curl_samples": len(contract["samples"]),
            "android_build_and_sample_unit_tests": True,
        },
        "backend": contract["backend"],
        "curl_pin": load(ROOT / "third_party/curl.lock.json"),
        "picosha2_pin": load(ROOT / "third_party/picosha2.lock.json"),
        "toolchain": {
            "cmake": subprocess.check_output(["cmake", "--version"], text=True).splitlines()[0],
            "cxx": subprocess.check_output(["g++", "--version"], text=True).splitlines()[0],
            "python": platform.python_version(), "zlib_headers": zlib_version + " (host); Android system libz",
        },
        "profiles": {"host": host_profile, "without_curl": profile(args.without_curl_build), "size": size["commands"]},
        "source_sha256": sources,
        "artifacts": [{"file": path.relative_to(ROOT).as_posix(), "bytes": path.stat().st_size,
                       "sha256": sha256(path)} for path in artifacts],
    }
    args.output.mkdir(parents=True, exist_ok=True)

    def write(name, value):
        (args.output / name).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    write("protocol-windows-pinned.json", protocol)
    write("recovery-windows.json", recovery)
    write("m3-public-api-windows.json", m3)
    write("size-arm64.json", size)
    write("curl-contract-windows-pinned.json", {key: value for key, value in contract.items() if key != "samples"} | {
        "sample_count": len(contract["samples"]),
        "raw_report_sha256": sha256(args.host_build / "reports/curl-contract.json"),
    })
    shutil.copyfile(ROOT / "build/m3-pinned-ctest.log", args.output / "ctest-windows.txt")
    shutil.copyfile(ROOT / "build/m3-no-curl-ctest.log", args.output / "ctest-without-curl.txt")
    (args.output / "scheduler-windows.txt").write_text(scheduler.group(0) + "\n", encoding="utf-8")
    start = android.find("releaseRuntimeClasspath -")
    end = android.find("A web-based", start)
    check(start >= 0, "Android dependency tree")
    (args.output / "android-dependencies.txt").write_text(
        android[start:end if end >= 0 else None].strip() + "\n", encoding="utf-8"
    )
    write("reference-build.json", manifest)
    print(json.dumps(manifest["validation"], ensure_ascii=False))
    print("Archived", len(sources), "source/build inputs")


if __name__ == "__main__":
    main()
