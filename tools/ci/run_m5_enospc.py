"""Real Linux ENOSPC in a private user/mount namespace; never mounts over user data."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/server"))
from http_fixture import RunningFixture
from m3_fixture import M3Handler


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
    with tempfile.TemporaryDirectory(prefix="enospc-", dir=args.output) as temporary, RunningFixture(handler=M3Handler) as fixture:
        subprocess.run(["unshare", "--user", "--map-root-user", "--mount", sys.executable, str(Path(__file__).resolve()),
            "--cli", str(args.cli.resolve()), "--output", str(args.output), "--namespace-child", temporary,
            fixture.url + "/m3/stream?bytes=16777216"], check=True, timeout=120)


if __name__ == "__main__":
    main()
