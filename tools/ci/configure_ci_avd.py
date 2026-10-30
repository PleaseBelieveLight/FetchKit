"""仅配置本工作流新建的 AVD，拒绝 avdmanager 生成的错误平台目标。"""
import argparse
import json
import os
from pathlib import Path
import re


def configure(avd_home, api):
    if not re.fullmatch(r"\d+(?:\.\d+)?", api):
        raise ValueError("无效的 Android 平台版本")
    metadata = avd_home / "fetchkit-ci.ini"
    values = dict(line.split("=", 1) for line in metadata.read_text(encoding="utf-8").splitlines() if "=" in line)
    target = values.get("target", "")
    if target != "android-" + api:
        raise ValueError("AVD target 与所选平台不一致；请检查 Command-line Tools 版本")
    config = avd_home / "fetchkit-ci.avd/config.ini"
    lines = config.read_text(encoding="utf-8").splitlines()
    # 当前 Action 的 heap-size 写入 hw.heapSize，实际 Dalvik 配置需使用 vm.heapSize。
    lines = [line for line in lines if not line.startswith("vm.heapSize=")]
    config.write_text("\n".join(lines) + "\nvm.heapSize=512\n", encoding="utf-8")
    return {"passed": True, "target": target, "dalvik_heap_mb": 512}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--api", required=True)
    args = parser.parse_args()
    if os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("FETCHKIT_CI_AVD") != "fetchkit-ci":
        raise RuntimeError("仅允许配置 GitHub Actions 新建的 fetchkit-ci AVD")
    avd_home = Path(os.environ["ANDROID_AVD_HOME"])
    output = Path("build/ci")
    output.mkdir(parents=True, exist_ok=True)
    report = {"passed": False, "expected_target": "android-" + args.api}
    try:
        report.update(configure(avd_home, args.api))
    finally:
        # 在启动之前保留根 ini 的目标，错误目标不能通过手工改值绕过检查。
        metadata = avd_home / "fetchkit-ci.ini"
        if metadata.exists():
            report["target_lines"] = [line for line in metadata.read_text(encoding="utf-8").splitlines()
                                      if line.startswith("target=")]
        (output / "emulator-target.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report))


if __name__ == "__main__":
    main()
