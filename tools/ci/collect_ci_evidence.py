"""归档单次 CI 原始输出及指纹；不生成正式发布验收通过标记。"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", required=True)
    parser.add_argument("--status", required=True, choices=("success", "failure", "cancelled", "skipped"))
    parser.add_argument("--input", action="extend", nargs="+", type=Path, default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    logs = []
    missing = []
    for index, source in enumerate(args.input):
        if not source.exists():
            missing.append(source.as_posix())
            continue
        paths = [source] if source.is_file() else sorted(source.rglob("*"))
        for path in paths:
            if not path.is_file() or path.is_symlink() or path.suffix not in (".json", ".xml", ".log", ".txt"):
                continue
            relative = Path(path.name) if source.is_file() else path.relative_to(source)
            target = args.output / "raw" / f"{index}-{source.name}" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            with target.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            logs.append({"file": target.relative_to(args.output).as_posix(), "sha256": digest})
    # 只选择可公开的标识；不遍历环境变量，也不收集凭证、请求体或用户级 Gradle 配置。
    run = {key: os.environ.get(key) for key in ("GITHUB_REPOSITORY", "GITHUB_SHA", "GITHUB_REF",
           "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "RUNNER_OS", "RUNNER_ARCH", "ImageOS", "ImageVersion")}
    report = {"profile": args.profile, "job_status": args.status, "release_acceptance": False,
              "recorded_at": datetime.now(timezone.utc).isoformat(), "run": run,
              "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "working_tree_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
              "environment": {"platform": platform.system(), "version": platform.platform(),
                              "machine": platform.machine(), "python": platform.python_version()},
              "missing_inputs": missing, "logs": logs}
    (args.output / "run.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
