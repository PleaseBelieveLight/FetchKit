"""Real Linux ENOSPC in a private user/mount namespace; never mounts over user data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--namespace-child", nargs=2, metavar=("MOUNTPOINT", "URL"), help=argparse.SUPPRESS)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Real ENOSPC acceptance requires Linux with unshare/mount and user namespaces")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.namespace_child:
        target = Path(args.namespace_child[0]).resolve()
        if target.parent != args.output or not target.name.startswith("enospc-"):
            raise RuntimeError("Unsafe mountpoint")
        subprocess.run(["mount", "--make-rprivate", "/"], check=True)
        subprocess.run(["mount", "-t", "tmpfs", "-o", "size=8m", "fetchkit-enospc", str(target)], check=True)
        try:
            result = subprocess.run([str(args.cli.resolve()), "download", args.namespace_child[1], "--root", str(target / "store"),
                "--allow-http", "--max-bytes", "33554432", "--capacity", "67108864", "--retries", "0"],
                capture_output=True, text=True, timeout=90)
            snapshot = json.loads(result.stdout)
            passed = result.returncode == 6 and snapshot["error"] == "DiskFull" and not snapshot["published"]
            report = {"passed": passed, "real_filesystem_enospc": True, "platform": "Linux", "filesystem": "private tmpfs 8 MiB",
                      "requested_bytes": 16777216, "exit_code": result.returncode, "snapshot": snapshot}
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            if not passed:
                raise RuntimeError("Real ENOSPC did not reject publication with DiskFull")
        finally:
            subprocess.run(["umount", str(target)], check=True)
        return
    # 子进程只使用标准库；映射后的 root 不能依靠宿主 DAC 权限穿越 runner 的私有 home。
    # 将只读输入与实验输出放入控制器拥有的系统临时目录，避免改动工作区/宿主目录权限。
    sys.path.insert(0, str(ROOT / "tests/server"))
    from http_fixture import RunningFixture
    from m3_fixture import M3Handler
    with tempfile.TemporaryDirectory(prefix="fetchkit-enospc-") as temporary, RunningFixture(handler=M3Handler) as fixture:
        workspace = Path(temporary).resolve()
        if workspace.parent != Path(tempfile.gettempdir()).resolve():
            raise RuntimeError("Unsafe temporary workspace")
        child_script, child_cli = workspace / "controller.py", workspace / "fetchkit_cli"
        shutil.copy2(Path(__file__).resolve(), child_script)
        shutil.copy2(args.cli.resolve(), child_cli)
        child_output = workspace / "result"
        child_output.mkdir()
        mountpoint = child_output / "enospc-volume"
        mountpoint.mkdir()
        subprocess.run(["unshare", "--user", "--map-root-user", "--mount", sys.executable, str(child_script),
            "--cli", str(child_cli), "--output", str(child_output), "--namespace-child", str(mountpoint),
            fixture.url + "/m3/stream?bytes=16777216"], cwd=workspace, check=True, timeout=120)
        report = json.loads((child_output / "report.json").read_text(encoding="utf-8"))
        if report.get("passed") is not True:
            raise RuntimeError("Incomplete real ENOSPC evidence")
        report.update({"controller_euid": os.geteuid(), "controller_python": platform.python_version(),
                       "cli_sha256": hashlib.sha256(child_cli.read_bytes()).hexdigest()})
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
