"""检查实际构建的版本文件名、APK 元数据和 Maven 主产物一致性。"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import xml.etree.ElementTree as ET
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/ci"))
from android_artifacts import android_artifact


def verify_artifacts(root):
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    artifacts = {}
    for kind in ("aar", "apk"):
        for build_type in ("debug", "release"):
            path = android_artifact(root, kind, build_type)
            artifacts[path.name] = {"bytes": path.stat().st_size,
                                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            if kind == "apk":
                metadata = json.loads((path.parent / "output-metadata.json").read_text(encoding="utf-8"))
                elements = metadata["elements"]
                if len(elements) != 1 or elements[0]["outputFile"] != path.name or elements[0]["versionName"] != version:
                    raise AssertionError("APK 元数据中的文件名和 versionName 必须与当前版本一致")
            with ZipFile(path) as archive:
                prefix = "jni" if kind == "aar" else "lib"
                for abi in ("arm64-v8a", "x86_64"):
                    if f"{prefix}/{abi}/libfetchkit_jni.so" not in archive.namelist():
                        raise AssertionError("产物缺少正式 ABI：" + abi)

    coordinates = root / f"build/m5-maven/com/luofelix/fetchkit/fetchkit/{version}"
    published = coordinates / f"fetchkit-{version}.aar"
    if published.read_bytes() != android_artifact(root, "aar").read_bytes():
        raise AssertionError("Maven 无 classifier 的主 AAR 必须与当前构建内容一致")
    ns = {"m": "http://maven.apache.org/POM/4.0.0"}
    pom = ET.parse(coordinates / f"fetchkit-{version}.pom")
    if pom.findtext("m:packaging", namespaces=ns) != "aar" or pom.findtext("m:version", namespaces=ns) != version:
        raise AssertionError("POM 必须声明当前版本和 aar 主产物")
    metadata = json.loads((coordinates / f"fetchkit-{version}.module").read_text(encoding="utf-8"))
    files = [item for variant in metadata["variants"] for item in variant.get("files", [])]
    expected = {f"fetchkit-{version}.aar", f"fetchkit-{version}-sources.jar"}
    if metadata["component"]["version"] != version or {item["url"] for item in files} != expected:
        raise AssertionError("Gradle module metadata 必须引用标准 Maven 文件名")
    for item in files:
        path = coordinates / item["url"]
        if item["sha256"] != hashlib.sha256(path.read_bytes()).hexdigest():
            raise AssertionError("Gradle module metadata 的文件指纹不一致")
    test_apk = root / "platform/android/sample/build/outputs/apk/androidTest/debug/sample-debug-androidTest.apk"
    if not test_apk.is_file():
        raise AssertionError("测试 APK 的默认路径必须继续可用")
    return {"passed": True, "version": version, "artifacts": artifacts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = verify_artifacts(ROOT)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
