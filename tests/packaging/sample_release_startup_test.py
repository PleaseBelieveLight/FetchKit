"""在明确选中的设备上验证真实 Release APK 的冷启动、首帧和页面入口。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = "com.luofelix.fetchkit"
COMPONENT = PACKAGE + "/com.luofelix.fetchkit.sample.MainActivity"


class AdbCommandError(RuntimeError):
    def __init__(self, operation, return_code, permission_denied=False):
        self.permission_denied = permission_denied
        reason = "，读取权限不足" if permission_denied else ""
        super().__init__(f"ADB 操作失败：{operation}，退出码 {return_code}{reason}")


def pull_installed_apk(command, remote, local, allow_emulator_root=False):
    try:
        command(["pull", remote, str(local)], timeout=60)
        return False
    except AdbCommandError as error:
        if not allow_emulator_root or not error.permission_denied:
            raise
    # 旧 userdebug 镜像可能禁止 shell 读取已安装 APK。只对明确授权的模拟器临时提权读取，
    # 无论读取结果如何均恢复 adbd；实际 Release 启动与页面验证仍在恢复后的环境进行。
    try:
        command(["root"])
        command(["wait-for-device"], timeout=120)
        command(["pull", remote, str(local)], timeout=60)
    finally:
        command(["unroot"])
        command(["wait-for-device"], timeout=120)
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--skip-install", action="store_true", help="验证设备上已安装的相同 APK")
    parser.add_argument("--emulator-root-pull", action="store_true", help="仅在测试模拟器读取 APK 权限不足时临时 root，读取后恢复")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    apk = args.apk.resolve()
    adb = [args.adb, "-s", args.serial]
    report = {"passed": False, "serial": args.serial, "checks": []}

    def command(arguments, timeout=30):
        result = subprocess.run(adb + arguments, capture_output=True, encoding="utf-8",
                                errors="replace", timeout=timeout)
        if result.returncode:
            raise AdbCommandError(arguments[0], result.returncode, "permission denied" in result.stderr.lower())
        return result.stdout

    def pid():
        result = subprocess.run(adb + ["shell", "pidof", PACKAGE], capture_output=True,
                                encoding="utf-8", timeout=15)
        return result.stdout.strip()

    def page(required, name):
        deadline = time.monotonic() + 25
        attempt = 0
        while time.monotonic() < deadline:
            if not pid():
                raise AssertionError("Release 应用在首帧或页面绘制期间退出")
            raw = command(["exec-out", "uiautomator", "dump", "/dev/tty"])
            start, end = raw.find("<hierarchy"), raw.rfind("</hierarchy>")
            if start >= 0 and end >= start:
                xml = raw[start:end + len("</hierarchy>")]
                (args.output / f"{name}-{attempt}.xml").write_text(xml, encoding="utf-8")
                tree = ET.fromstring(xml)
                texts = {node.get("text", "") for node in tree.iter("node") if node.get("package") == PACKAGE}
                if required <= texts:
                    return tree
            attempt += 1
            time.sleep(0.5)
        raise AssertionError(f"Release 页面未出现预期内容：{name}，请确认设备已解锁")

    def tap(tree, text):
        # 只点击本应用中明确匹配的文字；同名文字优先选择底部导航项。
        bounds = [tuple(map(int, re.findall(r"\d+", node.get("bounds", ""))))
                  for node in tree.iter("node") if node.get("package") == PACKAGE and node.get("text") == text]
        bounds = [value for value in bounds if len(value) == 4 and value[2] > value[0] and value[3] > value[1]]
        if not bounds:
            raise AssertionError("未找到页面入口：" + text)
        left, top, right, bottom = max(bounds, key=lambda value: value[1])
        command(["shell", "input", "tap", str((left + right) // 2), str((top + bottom) // 2)])

    try:
        version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        report.update({"version": version, "apk_sha256": hashlib.sha256(apk.read_bytes()).hexdigest(),
                       "api": command(["shell", "getprop", "ro.build.version.sdk"]).strip(),
                       "abis": command(["shell", "getprop", "ro.product.cpu.abilist"]).strip(),
                       "model": command(["shell", "getprop", "ro.product.model"]).strip(),
                       "emulator": command(["shell", "getprop", "ro.kernel.qemu"]).strip() == "1"})
        if args.emulator_root_pull and not (args.serial.startswith("emulator-") and report["emulator"]):
            raise ValueError("临时 root 读取仅允许明确选中的测试模拟器")
        if not args.skip_install:
            # 同签名覆盖安装保留数据；签名不匹配时失败，绝不卸载或清空用户数据。
            install = command(["install", "-r", str(apk)], timeout=120)
            (args.output / "install.txt").write_text(install, encoding="utf-8")
            if "Success" not in install:
                raise AssertionError("Release APK 覆盖安装失败")
        paths = command(["shell", "pm", "path", PACKAGE]).strip().splitlines()
        if len(paths) != 1 or not paths[0].startswith("package:"):
            raise AssertionError("预期安装单个独立 Release APK")
        installed = args.output / "installed.apk"
        report["apk_read_with_emulator_root"] = pull_installed_apk(
            command, paths[0][8:], installed, args.emulator_root_pull)
        if hashlib.sha256(installed.read_bytes()).hexdigest() != report["apk_sha256"]:
            raise AssertionError("设备实际安装的 APK 与待验证产物不同")
        package = command(["shell", "dumpsys", "package", PACKAGE])
        if re.search(r"\bDEBUGGABLE\b", package):
            raise AssertionError("必须验证非 debuggable 的 Release APK")
        home = {"拾流", "v" + version, "新建下载"}
        for index in range(3):
            command(["shell", "am", "force-stop", PACKAGE])
            launch = command(["shell", "am", "start", "-W", "-n", COMPONENT])
            (args.output / f"launch-{index}.txt").write_text(launch, encoding="utf-8")
            tree = page(home, f"home-{index}")
            time.sleep(1)
            if not pid():
                raise AssertionError("Release 冷启动后进程退出")
            report["checks"].append("cold-start-" + str(index + 1))
            if index == 0:
                tap(tree, "存储")
                tree = page({"资源与存储", "SDK 配额"}, "storage")
                report["checks"].append("storage")
                tap(tree, "能力指南")
                tree = page({"把能力跑一遍"}, "guide")
                report["checks"].append("guide")
                tap(tree, "下载")
                tree = page(home, "downloads")
                tap(tree, "新建下载")
                page({"清华镜像 · Netboot", "阿里云镜像 · Netboot"}, "new-download")
                report["checks"].append("new-download")
                command(["shell", "input", "keyevent", "KEYCODE_BACK"])
                page(home, "home-after-dialog")
        report["passed"] = True
    except Exception as error:
        report["error"] = type(error).__name__ + ": " + str(error)
        # 不清空设备日志；仅归档属于本应用进程的 crash buffer 行。
        try:
            crash = command(["logcat", "-b", "crash", "-d", "-v", "threadtime"])
            pids = set(re.findall(r"Process: " + re.escape(PACKAGE) + r", PID: (\d+)", crash))
            lines = [line for line in crash.splitlines()
                     if (match := re.match(r"\S+\s+\S+\s+(\d+)\s+", line)) and match.group(1) in pids]
            (args.output / "app-crash.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        except Exception as diagnostic_error:
            report["crash_collection_error"] = type(diagnostic_error).__name__
        raise
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report))


if __name__ == "__main__":
    main()
