"""只读等待明确选中的模拟器系统服务稳定，保留经过筛选的失败上下文。"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/packaging"))
from sample_release_startup_test import adb_failure_diagnostic


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    adb = ["adb", "-s", args.serial]
    report = {"passed": False, "system_server_pids": [], "required_stable_seconds": 30}

    def command(arguments):
        try:
            return subprocess.run(adb + arguments, capture_output=True, text=True,
                                  encoding="utf-8", errors="replace", timeout=15)
        except subprocess.TimeoutExpired:
            return subprocess.CompletedProcess([], 124, "", "")

    deadline = time.monotonic() + 180
    stable_since = None
    previous_pid = None
    try:
        while time.monotonic() < deadline:
            pid = command(["shell", "pidof", "system_server"]).stdout.strip()
            boot = command(["shell", "getprop", "sys.boot_completed"]).stdout.strip()
            package = command(["shell", "pm", "list", "packages", "android"])
            ready = (pid.isdecimal() and boot == "1" and package.returncode == 0
                     and "package:android" in package.stdout.splitlines())
            if pid.isdecimal() and pid != previous_pid:
                report["system_server_pids"].append(pid)
            if not ready or pid != previous_pid:
                stable_since = time.monotonic() if ready else None
            if stable_since is not None and time.monotonic() - stable_since >= 30:
                report["passed"] = True
                return
            previous_pid = pid
            time.sleep(2)
        raise RuntimeError("Android 系统服务在 180 秒内未达到连续 30 秒稳定状态")
    finally:
        crash = command(["logcat", "-b", "crash", "-d", "-v", "threadtime"])
        report["platform_crash_diagnostic"] = adb_failure_diagnostic(crash)
        report["properties"] = {name: command(["shell", "getprop", name]).stdout.strip()
                                for name in ("ro.build.version.sdk", "dalvik.vm.heapsize",
                                             "dalvik.vm.heapgrowthlimit", "sys.boot_completed", "init.svc.zygote")}
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report))


if __name__ == "__main__":
    main()
