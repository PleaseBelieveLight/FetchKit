"""根据 VERSION 精确定位当前变体的 Android 产物；不回退到旧版本或通配符。"""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
VERSION_PATTERN = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-(alpha|beta|rc)\.(0|[1-9][0-9]*))?")


def android_artifact(root, kind, build_type="release", *, require_exists=True):
    if kind not in ("aar", "apk") or build_type not in ("debug", "release"):
        raise ValueError("产物类型必须为 aar/apk，构建类型必须为 debug/release")
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    if not VERSION_PATTERN.fullmatch(version):
        raise ValueError("VERSION 必须为 major.minor.patch，可附加 -alpha.N、-beta.N 或 -rc.N")
    if kind == "aar":
        path = root / f"platform/android/sdk/build/outputs/aar/fetchkit-{version}-{build_type}.aar"
    else:
        path = root / f"platform/android/sample/build/outputs/apk/{build_type}/fetchkit-sample-{version}-{build_type}.apk"
    if require_exists and not path.is_file():
        raise FileNotFoundError(f"缺少当前版本的 {build_type} {kind} 产物：{path}")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("aar", "apk"), required=True)
    parser.add_argument("--build-type", choices=("debug", "release"), default="release")
    args = parser.parse_args()
    print(android_artifact(ROOT, args.kind, args.build_type))


if __name__ == "__main__":
    main()
