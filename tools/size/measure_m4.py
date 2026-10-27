"""Measure the complete production Android AAR in a minimal external file-AAR host."""
import argparse
import hashlib
import json
import io
from pathlib import Path
import platform
import shutil
import subprocess
import sys
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "build/m4-size-arm64")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    commands = []

    def gradle(arguments, log):
        command = (["cmd.exe", "/d", "/c", str(ROOT / "gradlew.bat")] if sys.platform == "win32"
                   else ["sh", str(ROOT / "gradlew")]) + arguments + ["--max-workers=2"]
        commands.append(command)
        with log.open("w", encoding="utf-8") as stream:
            subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, timeout=900, check=True)

    def describe(path):
        result = {"file": path.relative_to(output).as_posix(), "bytes": path.stat().st_size,
                  "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        with ZipFile(path) as archive:
            result["native"] = [{"path": item.filename, "bytes": item.file_size, "zip_bytes": item.compress_size}
                                for item in archive.infolist() if item.filename.endswith(".so")]
        return result

    gradle([":sdk:assembleRelease", "-Pfetchkit.probes=false"], output / "sdk-build.log")
    aar = output / "fetchkit-production.aar"
    shutil.copyfile(ROOT / "platform/android/sdk/build/outputs/aar/sdk-release.aar", aar)
    gradle([":sdk:dependencies", "--configuration", "releaseRuntimeClasspath", "-Pfetchkit.probes=false"],
           output / "sdk-dependencies.txt")
    report = {"platform": platform.platform(), "abi": "arm64-v8a", "api": 24, "build_type": "Release",
              "kotlin_in_bare_host": False, "stl": "c++_static included in JNI", "complete_android_download_cost": True,
              "scope": "Minimal Java Activity, real production file AAR and declared OkHttp transitive dependencies; no Compose/WorkManager",
              "base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "ordinary_aar": describe(aar), "profiles": {}, "commands": commands}
    for name, mode, r8 in [("bare", "bare", False), ("bare-r8", "bare", True), ("production", "production", False), ("production-r8", "production", True)]:
        destination = output / name
        destination.mkdir(exist_ok=True)
        properties = [f"-Pfetchkit.sizeMode={mode}", "-Pfetchkit.probes=false", "-Pfetchkit.abis=arm64-v8a",
                      f"-Pfetchkit.aar={aar.as_posix()}", f"-Pfetchkit.r8={str(r8).lower()}"]
        gradle([":size-host:assembleRelease"] + properties, destination / "build.log")
        gradle([":size-host:dependencies", "--configuration", "releaseRuntimeClasspath"] + properties,
               destination / "dependencies.txt")
        candidates = list((ROOT / "tools/size/android-host/build/outputs/apk/release").glob("*-unsigned.apk"))
        if len(candidates) != 1:
            raise RuntimeError("Expected one unsigned measurement APK")
        apk = destination / "host.apk"
        shutil.copyfile(candidates[0], apk)
        report["profiles"][name] = describe(apk) | {"r8": r8, "signed": False}
        if r8:
            mapping = ROOT / "tools/size/android-host/build/outputs/mapping/release/mapping.txt"
            if not mapping.is_file():
                raise RuntimeError("Missing R8 mapping")
            shutil.copyfile(mapping, destination / "mapping.txt")
    report["unshrunk_apk_delta_bytes"] = report["profiles"]["production"]["bytes"] - report["profiles"]["bare"]["bytes"]
    report["r8_apk_delta_bytes"] = report["profiles"]["production-r8"]["bytes"] - report["profiles"]["bare-r8"]["bytes"]
    prebuilt = next((args.ndk / "toolchains/llvm/prebuilt").iterdir())
    readelf = prebuilt / "bin" / ("llvm-readelf.exe" if sys.platform == "win32" else "llvm-readelf")
    report["jni"] = []
    with ZipFile(aar) as archive:
        if any("probe" in name.lower() for name in archive.namelist() if not name.endswith("/")):
            raise RuntimeError("Probe entered production AAR")
        with ZipFile(io.BytesIO(archive.read("classes.jar"))) as classes:
            if any("probe" in name.lower() for name in classes.namelist() if name.endswith(".class")):
                raise RuntimeError("Probe class entered production AAR")
        for name in archive.namelist():
            if not name.endswith(".so"):
                continue
            raw = archive.read(name)
            if b"FETCHKIT_FAULT" in raw:
                raise RuntimeError("Fault hooks entered production AAR")
            destination = output / Path(name).parent.name
            destination.mkdir(exist_ok=True)
            library = destination / Path(name).name
            library.write_bytes(raw)
            dynamic = subprocess.check_output([str(readelf), "-d", str(library)], text=True)
            symbols = subprocess.check_output([str(readelf), "--dyn-syms", str(library)], text=True)
            (destination / "dynamic.txt").write_text(dynamic, encoding="utf-8")
            (destination / "exports.txt").write_text(symbols, encoding="utf-8")
            exported = [line.split()[-1] for line in symbols.splitlines()
                        if " GLOBAL " in line and " DEFAULT " in line and " UND " not in line]
            if exported != ["JNI_OnLoad"]:
                raise RuntimeError("Unexpected JNI exports: " + repr(exported))
            report["jni"].append({"path": name, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(), "exports": exported})
    report["native_profiles"] = {}
    for abi in ("arm64-v8a", "x86_64"):
        profiles = []
        for cache in (ROOT / "platform/android/sdk/.cxx").glob(f"*/*/{abi}/CMakeCache.txt"):
            values = {}
            for line in cache.read_text(encoding="utf-8").splitlines():
                if not line.startswith(("#", "//")) and "=" in line and ":" in line.split("=", 1)[0]:
                    key, value = line.split("=", 1)
                    values[key.split(":", 1)[0]] = value
            if values.get("CMAKE_BUILD_TYPE") not in ("Release", "RelWithDebInfo"):
                continue
            if values.get("FETCHKIT_ENABLE_PLATFORM_PROBE", "").lower() in ("on", "true", "1"):
                continue
            archive = cache.parent / "fetchkit-core/libfetchkit.a"
            if archive.is_file():
                profile = {key: values.get(key, "") for key in ("CMAKE_BUILD_TYPE", "CMAKE_CXX_FLAGS",
                    "CMAKE_CXX_FLAGS_RELWITHDEBINFO", "CMAKE_CXX_FLAGS_RELEASE", "ANDROID_STL", "FETCHKIT_WITH_CURL",
                    "FETCHKIT_ENABLE_FAULT_INJECTION", "FETCHKIT_ENABLE_PLATFORM_PROBE")}
                profile["core_static_archive_bytes"] = archive.stat().st_size
                profile["core_static_archive_sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
                profiles.append((cache.stat().st_mtime, profile))
        if not profiles:
            raise RuntimeError("Missing ordinary native profile: " + abi)
        report["native_profiles"][abi] = max(profiles, key=lambda item: item[0])[1]
    sources = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT).decode().split("\0")
    report["source_sha256"] = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources
        if name and (name.startswith(("src/", "include/", "platform/android/", "third_party/", "tools/size/")) or
                     name in ("CMakeLists.txt", "settings.gradle.kts", "gradle/libs.versions.toml")) and (ROOT / name).is_file()}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("unshrunk_apk_delta_bytes", "r8_apk_delta_bytes", "jni")}, indent=2))


if __name__ == "__main__":
    main()
