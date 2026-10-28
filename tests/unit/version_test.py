"""校验实际 CLI 的完整版本，防止预发布后缀在 CMake 中丢失。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--version-file", type=Path, required=True)
    args = parser.parse_args()
    expected = args.version_file.read_text(encoding="utf-8").strip()
    result = subprocess.run([str(args.cli.resolve()), "--version"], check=True,
                            capture_output=True, text=True, timeout=5)
    actual = result.stdout.strip()
    if actual != expected:
        raise AssertionError(f"CLI 版本不一致：期望 {expected!r}，实际 {actual!r}")
    print(actual)


if __name__ == "__main__":
    main()
