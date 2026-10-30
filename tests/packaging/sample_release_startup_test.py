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


def node_has_label(node, label):
    return node.get("package") == PACKAGE and label in (node.get("text", ""), node.get("content-desc", ""))


def visible_labels(tree):
    # Compose 合并语义后，按钮名称可能通过 content-desc 暴露；仍限定为当前应用的实际节点。
    return {node.get(field, "") for node in tree.iter("node") if node.get("package") == PACKAGE
            for field in ("text", "content-desc")}


def adb_failure_code(output):
    # 仅保留平台机器码，不将 ADB 原始错误文本或路径带入通用诊断。
    match = re.search(r"\b(?:INSTALL_[A-Z_]+|DELETE_FAILED_[A-Z_]+)\b", output)
    if match:
        return match.group(0)
    return "Unknown"


def adb_failure_diagnostic(result):
    # 只提取预定义边界和异常类型；原始异常消息、堆栈、路径及请求内容不进入报告。
    output = result.stdout + result.stderr
    markers = ("Can't find service", "Failed transaction", "Permission denied", "Security exception",
               "No space left", "device offline", "device not found", "protocol fault",
               "Broken pipe", "Unknown command", "Error accessing file", "Error: Can't open file",
               "Performing Push Install", "Failure", "Success", "Watchdog", "Failed to allocate", "Fatal signal")
    return {"returncode": result.returncode, "platform_code": adb_failure_code(output),
            "exception_types": sorted(set(re.findall(r"\b(?:[a-z]\w*\.)+[A-Z]\w*(?:Exception|Error)\b", output))),
            "markers": [marker for marker in markers if marker.lower() in output.lower()],
            "stdout_bytes": len(result.stdout.encode("utf-8")),
            "stderr_bytes": len(result.stderr.encode("utf-8"))}


def read_installed_apk(adb, remote, local):
    # exec-out 保留二进制字节，不依赖旧镜像的 adb 文件同步接口，也不经过文本解码或 PTY。
    # 读取错误仍然失败，成功后仍由调用方与候选 APK 做完整 SHA-256 比对。
    with local.open("wb") as output:
        result = subprocess.run(adb + ["exec-out", "cat", remote], stdout=output,
                                stderr=subprocess.PIPE, timeout=60)
    if result.returncode:
        raise RuntimeError(f"读取设备 APK 失败，退出码 {result.returncode}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--skip-install", action="store_true", help="验证设备上已安装的相同 APK")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    apk = args.apk.resolve()
    adb = [args.adb, "-s", args.serial]
    report = {"passed": False, "serial": args.serial, "checks": []}

    def command(arguments, timeout=30):
        result = subprocess.run(adb + arguments, capture_output=True, encoding="utf-8",
                                errors="replace", timeout=timeout)
        if result.returncode:
            report["adb_failure"] = {"operation": arguments[0], **adb_failure_diagnostic(result)}
            code = adb_failure_code(result.stdout + result.stderr)
            raise RuntimeError(f"ADB 操作失败：{arguments[0]}，退出码 {result.returncode}，平台码 {code}")
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
                if required <= visible_labels(tree):
                    return tree
            attempt += 1
            time.sleep(0.5)
        raise AssertionError(f"Release 页面未出现预期内容：{name}，请确认设备已解锁")

    def tap(tree, text):
        # 只点击本应用中明确匹配的文字；同名文字优先选择底部导航项。
        bounds = [tuple(map(int, re.findall(r"\d+", node.get("bounds", ""))))
                  for node in tree.iter("node") if node_has_label(node, text)]
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
        if not args.skip_install:
            # 同签名覆盖安装保留数据；签名不匹配时失败，绝不卸载或清空用户数据。
            install = command(["install", "--no-streaming", "-r", str(apk)], timeout=120)
            (args.output / "install.txt").write_text(install, encoding="utf-8")
            if "Success" not in install:
                raise AssertionError("Release APK 覆盖安装失败")
        paths = command(["shell", "pm", "path", PACKAGE]).strip().splitlines()
        if len(paths) != 1 or not paths[0].startswith("package:"):
            raise AssertionError("预期安装单个独立 Release APK")
        installed = args.output / "installed.apk"
        read_installed_apk(adb, paths[0][8:], installed)
        report["apk_read_method"] = "exec-out-cat"
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
            report["platform_crash_diagnostic"] = adb_failure_diagnostic(
                subprocess.CompletedProcess([], 0, crash, ""))
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
