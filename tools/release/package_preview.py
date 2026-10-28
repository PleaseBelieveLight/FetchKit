"""Archive local production artifacts, matching native symbols and license metadata. No upload."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[2]


def fingerprint(path):
    return {"bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native-prefix", type=Path, required=True)
    parser.add_argument("--native-build", type=Path, required=True)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    version = (ROOT / "VERSION").read_text().strip()
    native_cli = args.native_prefix / "bin" / ("fetchkit_cli.exe" if sys.platform == "win32" else "fetchkit_cli")
    if subprocess.check_output([str(native_cli.resolve()), "--version"], text=True).strip() != version:
        raise RuntimeError("Native CLI version differs from publication")
    imports_command = ["objdump", "-p", str(native_cli.resolve())] if sys.platform == "win32" else ["ldd", str(native_cli.resolve())]
    imports = subprocess.check_output(imports_command, text=True)
    (args.output / "native-runtime-imports.txt").write_text(imports, encoding="utf-8")
    cache = (args.native_build / "CMakeCache.txt").read_text()
    if not re.search(r"FETCHKIT_ENABLE_FAULT_INJECTION:BOOL=OFF", cache) or not re.search(r"CMAKE_BUILD_TYPE:STRING=Release", cache):
        raise RuntimeError("Native preview requires a production Release build without fault injection")
    coordinates = args.repository / "com/luofelix/fetchkit/fetchkit" / version
    aar = coordinates / f"fetchkit-{version}.aar"
    pom = coordinates / f"fetchkit-{version}.pom"
    sources = coordinates / f"fetchkit-{version}-sources.jar"
    for artifact in (aar, pom, sources, coordinates / f"fetchkit-{version}.module"):
        if not artifact.is_file():
            raise RuntimeError("Missing publication artifact: " + str(artifact))
    ns = {"m": "http://maven.apache.org/POM/4.0.0"}
    tree = ET.parse(pom)
    if tree.findtext("m:version", namespaces=ns) != version:
        raise RuntimeError("POM version mismatch")
    project_license = (ROOT / "LICENSE").read_bytes()
    license_url = "https://www.apache.org/licenses/LICENSE-2.0.txt"
    pom_licenses = tree.findall("m:licenses/m:license", ns)
    if len(pom_licenses) != 1 or pom_licenses[0].findtext("m:url", namespaces=ns) != license_url or \
            pom_licenses[0].findtext("m:name", namespaces=ns) != "The Apache License, Version 2.0" or \
            pom_licenses[0].findtext("m:distribution", namespaces=ns) != "repo":
        raise RuntimeError("POM must declare the selected Apache-2.0 project license")
    installed_license = args.native_prefix / "share/licenses/fetchkit/LICENSE"
    if not installed_license.is_file() or installed_license.read_bytes() != project_license:
        raise RuntimeError("Installed native package lost the project LICENSE")
    dependencies = {(item.findtext("m:groupId", namespaces=ns), item.findtext("m:artifactId", namespaces=ns)):
                    item.findtext("m:version", namespaces=ns) for item in tree.findall("m:dependencies/m:dependency", ns)}
    if dependencies.get(("com.squareup.okhttp3", "okhttp")) != "5.5.1":
        raise RuntimeError("POM lost production OkHttp dependency")
    shutil.copytree(args.repository, args.output / "maven", dirs_exist_ok=True)
    shutil.copytree(ROOT / "third_party/licenses", args.output / "licenses", dirs_exist_ok=True)
    shutil.copyfile(ROOT / "LICENSE", args.output / "licenses/FetchKit-LICENSE")
    prebuilt = next((args.ndk / "toolchains/llvm/prebuilt").iterdir())
    readelf = prebuilt / "bin" / ("llvm-readelf.exe" if sys.platform == "win32" else "llvm-readelf")

    def build_id(path):
        notes = subprocess.check_output([str(readelf), "-n", str(path)], text=True)
        match = re.search(r"Build ID: (\w+)", notes)
        if not match:
            raise RuntimeError("Missing ELF build ID")
        return match.group(1)

    native = []
    with ZipFile(aar) as archive, ZipFile(sources) as source_archive:
        if source_archive.read("META-INF/fetchkit/LICENSE") != project_license:
            raise RuntimeError("Sources JAR lost the project LICENSE")
        if any("probe" in name.lower() for name in source_archive.namelist()):
            raise RuntimeError("Probe source in publication")
        if any(name.lower().endswith((".pem", ".key", ".crt", ".exe")) or "/raw/" in name for name in archive.namelist()):
            raise RuntimeError("Test trust anchor or executable in AAR")
        with ZipFile(io.BytesIO(archive.read("classes.jar"))) as classes:
            if classes.read("META-INF/fetchkit/LICENSE") != project_license:
                raise RuntimeError("AAR lost the project LICENSE")
            if any("probe" in name.lower() for name in classes.namelist() if not name.endswith("/")):
                raise RuntimeError("Probe class in publication")
        libraries = [name for name in archive.namelist() if name.endswith(".so")]
        if sorted(libraries) != [f"jni/{abi}/libfetchkit_jni.so" for abi in ("arm64-v8a", "x86_64")]:
            raise RuntimeError("Unexpected AAR ABI/library inventory")
        for name in libraries:
            abi = Path(name).parent.name
            directory = args.output / "symbols" / abi
            directory.mkdir(parents=True, exist_ok=True)
            stripped = directory / "stripped.so"
            stripped.write_bytes(archive.read(name))
            if b"FETCHKIT_FAULT" in stripped.read_bytes():
                raise RuntimeError("Fault hook in production JNI")
            identity = build_id(stripped)
            candidates = (ROOT / "platform/android/sdk/build/intermediates/cxx/RelWithDebInfo").glob(f"*/obj/{abi}/libfetchkit_jni.so")
            matching = [path for path in candidates if build_id(path) == identity]
            if not matching:
                raise RuntimeError("No matching unstripped symbols: " + abi)
            unstripped = directory / "libfetchkit_jni.so"
            shutil.copyfile(max(matching, key=lambda path: path.stat().st_mtime), unstripped)
            sections = subprocess.check_output([str(readelf), "-S", str(unstripped)], text=True)
            if ".debug_info" not in sections:
                raise RuntimeError("Native symbol archive lacks debug info")
            native.append({"abi": abi, "build_id": identity, "stripped": fingerprint(stripped), "symbols": fingerprint(unstripped)})
            stripped.unlink()
    with ZipFile(args.output / f"fetchkit-native-{version}.zip", "w", ZIP_DEFLATED) as archive:
        for path in sorted(args.native_prefix.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(args.native_prefix).as_posix())
    # Retain authoritative metadata/embedded notices for the resolved runtime graph.
    graph = args.dependencies.read_text(encoding="utf-8")
    components = set()
    for group, artifact, declared, resolved in re.findall(r"([\w.\-]+):([\w.\-]+):([\w.\-]+)(?: -> ([\w.\-]+))?", graph):
        components.add((group, artifact, resolved or declared))
    metadata = []
    gradle_cache = Path.home() / ".gradle/caches/modules-2/files-2.1"
    for group, artifact, selected in sorted(components):
        directory = gradle_cache / group / artifact / selected
        notices = []
        declaration = []
        target = args.output / "licenses/maven" / group / artifact / selected
        for path in directory.glob("*/*.pom"):
            target.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target / path.name)
            pom_tree = ET.parse(path)
            declaration += [item.findtext("m:name", namespaces=ns) for item in pom_tree.findall("m:licenses/m:license", ns)]
        for path in directory.glob("*/*.jar"):
            with ZipFile(path) as archive:
                for name in archive.namelist():
                    if re.search(r"(?:^|/)(?:LICENSE|NOTICE)(?:[.\-]|$)", name, re.I) and not name.endswith("/"):
                        target.mkdir(parents=True, exist_ok=True)
                        safe_name = path.stem + "-" + name.replace("/", "_")
                        (target / safe_name).write_bytes(archive.read(name))
                        notices.append(safe_name)
        metadata.append({"component": f"{group}:{artifact}:{selected}", "declared_licenses": declaration, "embedded_notices": notices})
    for relative in ("NOTICE", "toolchains/llvm/prebuilt/" + prebuilt.name + "/NOTICE"):
        notice = args.ndk / relative
        if notice.is_file():
            shutil.copyfile(notice, args.output / "licenses/Android-NDK-NOTICE")
            break
    shutil.copyfile(args.dependencies, args.output / "runtime-dependencies.txt")
    (args.output / "dependency-licenses.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    source_names = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT).decode().split("\0")
    source_hashes = {name: fingerprint(ROOT / name)["sha256"] for name in source_names if name and (ROOT / name).is_file()
                     and name.startswith(("src/", "include/", "platform/android/sdk/", "cmake/", "third_party/", "gradle/",
                                          "examples/cli/", "examples/consumer/", "examples/android-consumer/"))}
    source_hashes.update({name: fingerprint(ROOT / name)["sha256"] for name in ("LICENSE", "VERSION", "CMakeLists.txt", "settings.gradle.kts")})
    manifest = {"version": version, "preview_only": True, "uploaded": False,
                "base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "working_tree_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
                "project_license_selected": True,
                "project_license": {"spdx_id": "Apache-2.0", "url": license_url, "sha256": hashlib.sha256(project_license).hexdigest()},
                "native_symbols": native, "source_sha256": source_hashes,
                "artifacts": {path.relative_to(args.output).as_posix(): fingerprint(path) for path in sorted(args.output.rglob("*"))
                              if path.is_file() and path.name != "manifest.json"}}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"version": version, "preview_only": True, "symbols": len(native), "project_license_selected": manifest["project_license_selected"]}))


if __name__ == "__main__":
    main()
