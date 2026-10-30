"""从当前构建配置选择 CI 工具链，不维护第二份版本快照。"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[2]


def configuration(root=ROOT):
    versions = tomllib.loads((root / "gradle/libs.versions.toml").read_text(encoding="utf-8"))["versions"]
    daemon = (root / "gradle/gradle-daemon-jvm.properties").read_text(encoding="utf-8")
    java = re.search(r"^toolchainVersion=(\d+)$", daemon, re.M)
    apis = []
    for module in ("sdk", "sample"):
        source = (root / f"platform/android/{module}/build.gradle.kts").read_text(encoding="utf-8")
        match = re.search(r"compileSdk\s*\{\s*version\s*=\s*release\((\d+)\)", source)
        if not match:
            raise ValueError("无法读取 compileSdk：" + module)
        apis.append(match.group(1))
    if not java or len(set(apis)) != 1:
        raise ValueError("缺少 JDK 版本，或 SDK/sample 的 compileSdk 不一致")
    return {"java": java.group(1), "api": apis[0], "ndk": versions["ndk"], "cmake": versions["cmake"]}


def available_packages(listing):
    return set(re.findall(r"^\s*([\w.;-]+)\s*\|", listing, re.M))


def platform_package(api, available):
    # 新 SDK 使用 android-37.0，旧 SDK 使用 android-31；依据仓库列表选择，避免猜测安装路径。
    platforms = [f"platforms;android-{api}.0", f"platforms;android-{api}"]
    return next((item for item in platforms if item in available), None)


def packages(config, listing):
    available = available_packages(listing)
    platform = platform_package(config["api"], available)
    builds = [item for item in available if re.fullmatch(r"build-tools;" + config["api"] + r"\.\d+\.\d+", item)]
    required = [f"ndk;{config['ndk']}", f"cmake;{config['cmake']}", "platform-tools"]
    if not platform or not builds or any(item not in available for item in required):
        raise ValueError("Android 仓库缺少当前构建所需的正式平台、build-tools、NDK 或 CMake")
    build = max(builds, key=lambda item: tuple(map(int, item.split(";")[1].split("."))))
    return [platform, build] + required


def emulator_packages(api, listing):
    available = available_packages(listing)
    platform = platform_package(str(api), available)
    images = [f"system-images;android-{api}.0;google_apis;x86_64", f"system-images;android-{api};google_apis;x86_64"]
    image = next((item for item in images if item in available), None)
    if not platform or not image:
        raise ValueError("缺少选定 API 的正式 x86_64 模拟器平台或镜像")
    return {"emulator-platform": platform.split("android-")[1], "emulator-image": image.split(";")[1].removeprefix("android-")}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--emulator-api", type=int)
    args = parser.parse_args()
    config = configuration()
    if args.install or args.emulator_api:
        manager = shutil.which("sdkmanager")
        if not manager:
            raise RuntimeError("PATH 中缺少 sdkmanager")
        listing = subprocess.check_output([manager, "--list"], text=True, encoding="utf-8", timeout=180)
        if args.emulator_api:
            config.update(emulator_packages(args.emulator_api, listing))
        if args.install:
            selected = packages(config, listing)
            subprocess.run([manager] + selected, check=True, timeout=1200)
            directory = ROOT / "build/ci"
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "android-toolchain.txt").write_text("\n".join(selected) + "\n", encoding="utf-8")
    if args.output:
        with args.output.open("a", encoding="utf-8") as output:
            output.writelines(f"{key}={value}\n" for key, value in config.items())
    print(config)


if __name__ == "__main__":
    main()
