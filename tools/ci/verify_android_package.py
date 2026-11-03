"""Build a standalone Maven consumer with R8, then run a trusted-digest JNI download."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/server"))
from http_fixture import CONTENT, RunningFixture
from android_reverse import allocate_reverse


def build_consumer(args, version, report):
    with tempfile.TemporaryDirectory(prefix="fetchkit-android-consumer-") as temporary:
        workspace = Path(temporary).resolve()
        if ROOT == workspace or ROOT in workspace.parents:
            raise RuntimeError("External consumer must be outside source tree")
        shutil.copytree(ROOT / "examples/android-consumer", workspace, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("build", ".gradle", ".cxx", "local.properties"))
        # Preserve the repository's tested daemon toolchain, including a complete jlink.
        (workspace / "gradle").mkdir(exist_ok=True)
        shutil.copyfile(ROOT / "gradle/gradle-daemon-jvm.properties", workspace / "gradle/gradle-daemon-jvm.properties")
        (workspace / "local.properties").write_text("sdk.dir=" + args.sdk.resolve().as_posix() + "\n")
        command = (["cmd.exe", "/d", "/c", str(ROOT / "gradlew.bat")] if sys.platform == "win32" else ["sh", str(ROOT / "gradlew")])
        command += ["-p", str(workspace), "assembleRelease", "dependencies", "--configuration", "releaseRuntimeClasspath",
                    "-Pfetchkit.repository=" + args.repository.resolve().as_uri(), "-Pfetchkit.version=" + version,
                    "-Pfetchkit.abi=" + args.abi, f"--max-workers={args.max_workers}", "--no-daemon"]
        if args.coroutines_aar:
            # Standalone consumers declare the adapter AAR and its runtime dependency separately.
            adapter = workspace / "fetchkit-coroutines.aar"
            shutil.copyfile(args.coroutines_aar, adapter)
            catalog = (ROOT / "gradle/libs.versions.toml").read_text(encoding="utf-8")
            version_match = re.search(r'^coroutines\s*=\s*"([^"]+)"', catalog, re.MULTILINE)
            if version_match is None:
                raise RuntimeError("Missing coroutines version in the version catalog")
            command += ["-Pfetchkit.coroutinesAar=" + adapter.as_posix(),
                        "-Pfetchkit.coroutinesVersion=" + version_match.group(1)]
            report["coroutines_aar_sha256"] = hashlib.sha256(adapter.read_bytes()).hexdigest()
        if args.offline:
            command.append("--offline")
        with (args.output / "build.log").open("w", encoding="utf-8") as log:
            environment = os.environ.copy()
            if args.java_home:
                environment["JAVA_HOME"] = str(args.java_home.resolve())
            subprocess.run(command, cwd=workspace, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=900, check=True)
        apk = workspace / "build/outputs/apk/release/FetchKitExternalConsumer-release.apk"
        if not apk.is_file():
            candidates = list((workspace / "build/outputs/apk/release").glob("*.apk"))
            if len(candidates) != 1:
                raise RuntimeError("Expected a single Release APK")
            apk = candidates[0]
        shutil.copyfile(apk, args.output / "consumer-release.apk")
        shutil.copyfile(workspace / "build/outputs/mapping/release/mapping.txt", args.output / "mapping.txt")
        report["apk_sha256"] = hashlib.sha256(apk.read_bytes()).hexdigest()
        report["apk_bytes"] = apk.stat().st_size


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--adb")
    parser.add_argument("--serial")
    parser.add_argument("--java-home", type=Path, help="Complete JDK including jlink; overrides JAVA_HOME for this build")
    parser.add_argument("--offline", action="store_true", help="Use pre-resolved Gradle dependencies for reproducible local acceptance")
    parser.add_argument("--max-workers", type=int, default=2)
    parser.add_argument("--coroutines-aar", type=Path, help="Also exercise the optional adapter from a standalone Release AAR")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--build-only", action="store_true", help="Build and fingerprint the external APK without a device")
    mode.add_argument("--skip-build", action="store_true", help="Run an APK previously fingerprinted in the output directory")
    parser.add_argument("--abi", choices=["arm64-v8a", "x86_64"], required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.build_only and (not args.adb or not args.serial):
        parser.error("--adb and --serial are required for device verification")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    version = (ROOT / "VERSION").read_text().strip()
    adb = [args.adb, "-s", args.serial]
    package = "com.luofelix.fetchkit.sizehost"
    report = {"passed": False, "version": version, "abi": args.abi, "r8": True, "source_tree_dependency": False}
    rule = None
    try:
        apk = args.output / "consumer-release.apk"
        build_report = args.output / "build-report.json"
        if args.skip_build:
            built = json.loads(build_report.read_text(encoding="utf-8"))
            if not built.get("build_passed") or any(
                built.get(key) != report[key] for key in ("version", "abi", "r8", "source_tree_dependency")
            ):
                raise RuntimeError("External APK does not match the requested candidate profile")
            if built.get("apk_sha256") != hashlib.sha256(apk.read_bytes()).hexdigest():
                raise RuntimeError("External APK changed after the build")
            report.update(built)
        else:
            build_consumer(args, version, report)
        adapter_sha = hashlib.sha256(args.coroutines_aar.read_bytes()).hexdigest() if args.coroutines_aar else None
        if adapter_sha != report.get("coroutines_aar_sha256"):
            raise RuntimeError("External APK used a different coroutines adapter")
        published_aar = args.repository / "com/luofelix/fetchkit/fetchkit" / version / f"fetchkit-{version}.aar"
        publication_sha = hashlib.sha256(published_aar.read_bytes()).hexdigest()
        if args.skip_build and publication_sha != report.get("publication_aar_sha256"):
            raise RuntimeError("External APK used a different Maven publication")
        report["publication_aar_sha256"] = publication_sha
        with ZipFile(published_aar) as published, ZipFile(apk) as installed:
            expected = published.read(f"jni/{args.abi}/libfetchkit_jni.so")
            if installed.read(f"lib/{args.abi}/libfetchkit_jni.so") != expected:
                raise RuntimeError("External APK did not consume the current publication's native library")
        if not args.skip_build:
            report["build_passed"] = True
            build_report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        if args.build_only:
            return
        report["api"] = subprocess.check_output(adb + ["shell", "getprop", "ro.build.version.sdk"], text=True).strip()
        report["device_abis"] = subprocess.check_output(adb + ["shell", "getprop", "ro.product.cpu.abilist"], text=True).strip()
        result = subprocess.run(adb + ["install", "--no-streaming", "-r", str(apk)], capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError("Consumer install failed: " + result.stdout + result.stderr)
        with RunningFixture() as fixture:
            port, rule = allocate_reverse(adb, fixture.server.server_port)
            run_id = str(uuid.uuid4())
            subprocess.run(adb + ["shell", "am", "force-stop", package], check=True, timeout=20, capture_output=True)
            subprocess.run(adb + ["shell", "am", "start", "-W", "-n", package + "/.MainActivity",
                "--es", "fetchkit_url", f"http://127.0.0.1:{port}/range", "--es", "fetchkit_sha256", hashlib.sha256(CONTENT).hexdigest(),
                "--es", "fetchkit_run_id", run_id], timeout=30, check=True, capture_output=True)
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                output = subprocess.run(adb + ["exec-out", "cat", "/sdcard/Android/data/" + package + "/files/m4-release.json"],
                                        timeout=15, capture_output=True, text=True)
                try:
                    value = json.loads(output.stdout)
                    if value.get("run_id") == run_id:
                        break
                except ValueError:
                    pass
                time.sleep(0.5)
            else:
                raise RuntimeError("R8 consumer result deadline")
            if value.get("passed") is not True or value.get("bytes") != len(CONTENT) or value.get("events", 0) < 1:
                raise RuntimeError("R8 consumer download, event, lease or export assertions failed")
            if value.get("version") != version:
                raise RuntimeError("R8 consumer JNI version differs from publication")
            report["result"] = value
            report["passed"] = True
    finally:
        if rule:
            subprocess.run(adb + ["reverse", "--remove", rule], timeout=15, capture_output=True)
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
