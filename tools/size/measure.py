"""Build identical unsigned, unshrunk Release hosts and keep their complete inputs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[2]


def gradle(arguments, log):
    command = (["cmd.exe", "/d", "/c", str(ROOT / "gradlew.bat")] if sys.platform == "win32"
               else ["sh", str(ROOT / "gradlew")]) + arguments
    with log.open("w", encoding="utf-8") as output:
        completed = subprocess.run(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, timeout=600)
    if completed.returncode:
        raise RuntimeError(f"Gradle failed: {log}")


def describe(path):
    result = {"file": path.name, "bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    with ZipFile(path) as archive:
        result["native"] = [{"path": entry.filename, "bytes": entry.file_size, "zip_bytes": entry.compress_size}
                            for entry in archive.infolist() if entry.filename.endswith(".so")]
        assert all("test" not in entry.filename.lower() for entry in archive.infolist() if entry.filename.endswith(".so"))
    return result


def native_profile(abi, probe_enabled, destination):
    candidates = []
    for cache in (ROOT / "platform/android/sdk/.cxx").glob(f"*/*/{abi}/CMakeCache.txt"):
        values = {}
        for line in cache.read_text(encoding="utf-8").splitlines():
            if not line.startswith(("#", "//")) and "=" in line and ":" in line.split("=", 1)[0]:
                key, value = line.split("=", 1)
                values[key.split(":", 1)[0]] = value
        if values.get("CMAKE_BUILD_TYPE") not in ("Release", "RelWithDebInfo"):
            continue
        enabled = values.get("FETCHKIT_ENABLE_PLATFORM_PROBE", "").lower() in ("on", "true", "1")
        archive = cache.parent / "fetchkit-core/libfetchkit.a"
        if enabled == probe_enabled and archive.exists():
            candidates.append((cache.stat().st_mtime, archive, values))
    if not candidates:
        raise RuntimeError("Cannot locate the matching native core measurement")
    _, archive, values = max(candidates, key=lambda candidate: candidate[0])
    shutil.copy2(archive, destination / "libfetchkit.a")
    native_type = values["CMAKE_BUILD_TYPE"]
    return {"cmake_build_type": native_type, "core_static_archive_bytes": archive.stat().st_size,
            "core_static_archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "cxx_flags": values.get("CMAKE_CXX_FLAGS", ""),
            "configuration_flags": values.get(f"CMAKE_CXX_FLAGS_{native_type.upper()}", "")}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "build/m0-size")
    parser.add_argument("--abi", choices=["arm64-v8a", "x86_64"], default="arm64-v8a")
    args = parser.parse_args()
    destination = args.output.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    report = {"platform": platform.platform(), "abi": args.abi, "build_type": "Release",
              "r8": False, "signed": False, "kotlin_in_bare_host": False, "modes": {}}
    report["base_commit"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    report["working_tree"] = subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True)
    java = str(Path(os.environ["JAVA_HOME"]) / "bin/java") if os.environ.get("JAVA_HOME") else "java"
    report["launcher_java"] = subprocess.check_output([java, "-version"], stderr=subprocess.STDOUT, text=True)
    sources = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT).decode().split("\0")
    report["source_sha256"] = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources
        if name and not name.startswith("docs/") and ((ROOT / name).suffix in (".py", ".kt", ".kts", ".cpp", ".h", ".toml")
        or name.endswith("CMakeLists.txt") or name == "tests/fixtures/m0_scenarios.json")}
    for mode in ["bare", "scaffold", "probe", "okhttp"]:
        output = destination / mode
        output.mkdir(exist_ok=True)
        properties = [f"-Pfetchkit.sizeMode={mode}", f"-Pfetchkit.probes={str(mode in ('probe', 'okhttp')).lower()}",
                      f"-Pfetchkit.abis={args.abi}",
                      f"-Pfetchkit.probeBackend={'okhttp' if mode == 'okhttp' else 'platform'}"]
        tasks = [":size-host:assembleRelease"]
        if mode != "bare":
            tasks.append(":sdk:assembleRelease")
        gradle(tasks + properties, output / "build.log")
        gradle([":size-host:dependencies", "--configuration", "releaseRuntimeClasspath"] + properties,
               output / "dependencies.txt")
        source_apk = ROOT / "tools/size/android-host/build/outputs/apk/release/android-host-release-unsigned.apk"
        # AGP uses project name for the APK prefix, independent of physical directory.
        candidates = list(source_apk.parent.glob("*-release-unsigned.apk"))
        if len(candidates) != 1:
            raise RuntimeError("Expected exactly one unsigned size-host APK")
        apk = output / "host.apk"
        shutil.copy2(candidates[0], apk)
        result = {"apk": describe(apk), "command": tasks + properties}
        if mode != "bare":
            aar = output / "sdk.aar"
            shutil.copy2(ROOT / "platform/android/sdk/build/outputs/aar/sdk-release.aar", aar)
            result["aar"] = describe(aar)
            result["native_profile"] = native_profile(args.abi, mode in ("probe", "okhttp"), output)
        report["modes"][mode] = result
        result["apk_delta_from_bare"] = result["apk"]["bytes"] - report["modes"]["bare"]["apk"]["bytes"]
        print(f'{mode}: APK {result["apk"]["bytes"]} bytes, delta {result["apk_delta_from_bare"]}', flush=True)
        (destination / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    full_abi = destination / "scaffold-full"
    full_abi.mkdir(exist_ok=True)
    gradle([":sdk:assembleRelease", "-Pfetchkit.probes=false", "-Pfetchkit.abis=arm64-v8a,x86_64"], full_abi / "build.log")
    full_aar = full_abi / "sdk.aar"
    shutil.copy2(ROOT / "platform/android/sdk/build/outputs/aar/sdk-release.aar", full_aar)
    report["full_abi_scaffold_aar"] = describe(full_aar)
    (destination / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Report: {destination / 'report.json'}")


if __name__ == "__main__":
    main()
