"""通过真实 Gradle 发布任务验证拒绝路径；不上传任何产物。"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    gradle = (["cmd.exe", "/d", "/c", str(ROOT / "gradlew.bat")] if sys.platform == "win32"
              else ["sh", str(ROOT / "gradlew")])
    # 显式清空两项凭证，保证缺少认证的远程任务在上传前终止，不受本机凭证影响。
    flags = ["--max-workers=2", "--no-configuration-cache", "--console=plain", "-Pgpr.user=", "-Pgpr.key="]
    if args.offline:
        flags.append("--offline")
    # 只检查发布门槛，不让探针或单 ABI 的诊断构建覆盖此前验证的生产产物。
    for task in ("bundleReleaseAar", "sourceReleaseJar", "generatePomFileForReleasePublication",
                 "generateMetadataFileForReleasePublication"):
        flags += ["-x", ":sdk:" + task]
    cases = [
        ("probe", ":sdk:publishReleasePublicationToPreviewRepository", ["-Pfetchkit.probes=true"],
         "发布产物不得包含实验探针"),
        ("abi", ":sdk:publishReleasePublicationToPreviewRepository",
         ["-Pfetchkit.probes=false", "-Pfetchkit.abis=arm64-v8a"],
         "发布产物必须包含 arm64-v8a 和 x86_64"),
        ("credentials", ":sdk:publishReleasePublicationToGitHubPackagesRepository", ["-Pfetchkit.probes=false"],
         "GitHub Packages 发布需要认证"),
    ]
    results = []
    for name, task, properties, expected in cases:
        result = subprocess.run(gradle + [task] + flags + properties, cwd=ROOT,
                                capture_output=True, timeout=180)
        raw = result.stdout + result.stderr
        try:
            output = raw.decode("utf-8")
        except UnicodeDecodeError:
            # Windows 的 Gradle 控制台可能沿用 GBK，与文件编码设置不同。
            output = raw.decode("gb18030")
        log = args.output / (name + "-guard.log")
        log.write_text(output, encoding="utf-8")
        passed = result.returncode != 0 and expected in output
        results.append({"case": name, "passed": passed, "exit_code": result.returncode})
        (args.output / "publication-guards.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
        if not passed:
            raise AssertionError(f"发布拦截检查失败：{name}，详见 {log}")
    print(json.dumps(results))


if __name__ == "__main__":
    main()
