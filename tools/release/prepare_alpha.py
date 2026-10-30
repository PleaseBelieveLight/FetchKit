"""校验手动 Alpha Tag 并整理已验证的产物；本工具不上传。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[2]


def validate_tag(version, ref, commit, tagged_commit):
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-alpha\.(0|[1-9][0-9]*)", version):
        raise ValueError("该工作流只支持 Alpha；正式版继续使用严格发布验收")
    if ref != "refs/tags/v" + version or commit != tagged_commit:
        raise ValueError("必须从与 VERSION、当前提交一致的 v<VERSION> Tag 手动运行")


def fingerprint(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"bytes": path.stat().st_size, "sha256": digest}


def verify_candidate(preview, startup, consumer, aar, apk, version, commit):
    if preview["version"] != version or preview["base_commit"] != commit or preview["working_tree_dirty"]:
        raise ValueError("预览必须来自当前 Tag 的干净源码")
    if not startup.get("passed") or startup.get("version") != version or startup.get("apk_sha256") != fingerprint(apk)["sha256"]:
        raise ValueError("实际待分发 APK 缺少对应的 Release 启动通过记录")
    if not consumer.get("passed") or consumer.get("version") != version or consumer.get("publication_aar_sha256") != fingerprint(aar)["sha256"]:
        raise ValueError("实际待分发 AAR 缺少对应的外部 R8/JNI 接入通过记录")


def verify_apk_libraries(aar, apk):
    with ZipFile(aar) as sdk, ZipFile(apk) as sample:
        expected = {f"lib/{abi}/libfetchkit_jni.so" for abi in ("arm64-v8a", "x86_64")}
        # sample 的 Compose 等依赖也可携带 native 库；这里只约束 SDK 自身的 JNI 库。
        libraries = {name for name in sample.namelist() if name.endswith("/libfetchkit_jni.so")}
        if libraries != expected:
            raise ValueError("Release APK 必须包含两种正式 ABI 的 FetchKit JNI 库")
        for name in expected:
            if sample.read(name) != sdk.read(name.replace("lib/", "jni/", 1)):
                raise ValueError("Release APK 与已验证 AAR 的 JNI 内容不一致")
        if any(name.lower().endswith((".pem", ".key", ".crt")) for name in sample.namelist()):
            raise ValueError("Release APK 不得包含测试信任锚")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check-tag", action="store_true")
    parser.add_argument("--stage", action="store_true")
    args = parser.parse_args()
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    tagged = subprocess.check_output(["git", "rev-parse", "refs/tags/v" + version + "^{commit}"], cwd=ROOT, text=True).strip()
    validate_tag(version, os.environ.get("GITHUB_REF", ""), commit, tagged)
    if not (args.check_tag or args.stage):
        parser.error("选择 --check-tag 或 --stage")
    if not args.stage:
        return
    build = ROOT / "build"
    preview = build / "alpha-preview"
    coordinates = preview / f"maven/com/luofelix/fetchkit/fetchkit/{version}"
    aar = coordinates / f"fetchkit-{version}.aar"
    apk = ROOT / "platform/android/sample/build/outputs/apk/release/sample-release.apk"
    manifest = json.loads((preview / "manifest.json").read_text(encoding="utf-8"))
    startup = json.loads((build / "ci/release-startup/report.json").read_text(encoding="utf-8"))
    consumer = json.loads((build / "ci/aar-consumer/report.json").read_text(encoding="utf-8"))
    verify_candidate(manifest, startup, consumer, aar, apk, version, commit)
    verify_apk_libraries(aar, apk)
    # 核对归档中的每一份文件，防止测试之后误用另一批产物或修改过的符号/许可材料。
    for name, identity in manifest["artifacts"].items():
        path = (preview / name).resolve()
        if preview.resolve() not in path.parents or fingerprint(path) != identity:
            raise ValueError("预览文件指纹不一致：" + name)
    output = build / "alpha-assets"
    output.mkdir(exist_ok=False)
    for path in (aar, coordinates / f"fetchkit-{version}-sources.jar", coordinates / f"fetchkit-{version}.pom",
                 coordinates / f"fetchkit-{version}.module",
                 preview / f"fetchkit-native-{version}.zip"):
        shutil.copyfile(path, output / path.name)
    shutil.copyfile(apk, output / f"fetchkit-sample-{version}.apk")
    for directory in ("symbols", "licenses"):
        with ZipFile(output / f"fetchkit-{directory}-{version}.zip", "w", ZIP_DEFLATED) as archive:
            for path in sorted((preview / directory).rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(preview).as_posix())
    shutil.copyfile(preview / "manifest.json", output / "preview-manifest.json")
    shutil.copyfile(preview / "runtime-dependencies.txt", output / "runtime-dependencies.txt")
    result = {"version": version, "commit": commit, "release_acceptance": False,
              "artifacts": {path.name: fingerprint(path) for path in sorted(output.iterdir())}}
    (output / "alpha-manifest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    (output / "SHA256SUMS").write_text("".join(f"{fingerprint(path)['sha256']}  {path.name}\n"
        for path in sorted(output.iterdir()) if path.is_file()), encoding="utf-8")
    (build / "alpha-notes.md").write_text(
        f"FetchKit {version}\n\n源码提交：{commit}\n\n"
        "Alpha 预发布，C ABI 尚未冻结，升级后请重新编译宿主。\n\n"
        "包含双 ABI AAR、sources、POM、Linux Native 预览、公开演示签名的 Release APK、符号和 SHA-256。"
        "直接使用 AAR 仍需自行声明 OkHttp 等运行依赖。Linux Native 包依赖目标系统的 OpenSSL、zlib 和运行库。\n\n"
        "本次自动验证覆盖 Linux 常规协议/恢复、外部 C/C++ 接入、API 31 x86_64 模拟器的 Android 协议/恢复、"
        "外部 Release/R8 AAR 接入和实际 Release APK 冷启动。\n\n"
        "该流程不代表首版正式发布验收通过；未覆盖物理 arm64、完整 API 矩阵、物理重启/掉电、"
        "实际 >4 GiB、真实 ENOSPC、sanitizer 和完整第三方许可审核等独立门槛。详见仓库发布检查表。\n",
        encoding="utf-8")


if __name__ == "__main__":
    main()
