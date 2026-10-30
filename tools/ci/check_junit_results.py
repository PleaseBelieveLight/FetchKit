"""拒绝缺失、空集合或失败的 Android 单元测试结果。"""
import argparse
from pathlib import Path
import xml.etree.ElementTree as ET


def check_results(directories):
    total = 0
    for directory in directories:
        # 当前 AGP 配置只提供 Debug 单测；每个模块都必须实际执行，Release 另做真实 APK 回归。
        for variant in ("testDebugUnitTest",):
            files = sorted((directory / variant).glob("TEST-*.xml"))
            tests = 0
            for path in files:
                suite = ET.parse(path).getroot()
                count = int(suite.get("tests", "0"))
                skipped = int(suite.get("skipped", "0"))
                if int(suite.get("failures", "0")) or int(suite.get("errors", "0")):
                    raise ValueError("单元测试失败：" + str(path))
                tests += count - skipped
            if tests <= 0:
                raise ValueError("未执行有效单元测试：" + str(directory / variant))
            print(f"{directory.parent.parent.name}/{variant}: {tests} tests, {len(files)} classes")
            total += tests
    return total


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directories", type=Path, nargs="+")
    args = parser.parse_args()
    print(f"Total: {check_results(args.directories)} tests")


if __name__ == "__main__":
    main()
