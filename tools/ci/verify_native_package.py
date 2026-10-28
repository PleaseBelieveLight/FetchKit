"""Relocate an install, compile standalone C/C++ consumers and verify real bytes."""
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
sys.path.insert(0, str(ROOT / "tests/server"))
from http_fixture import RunningFixture


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dependency-prefix", type=Path, action="append", default=[])
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"passed": False, "platform": platform.platform(), "version": (ROOT / "VERSION").read_text().strip(),
              "relocated": True, "consumers": [], "commands": []}
    try:
        with tempfile.TemporaryDirectory(prefix="fetchkit-consumer-") as temporary:
            workspace = Path(temporary).resolve()
            if workspace == ROOT or ROOT in workspace.parents:
                raise RuntimeError("Consumer workspace must be outside the source tree")
            prefix = workspace / "relocated-install"
            shutil.copytree(args.prefix.resolve(), prefix)
            source = workspace / "consumer"
            shutil.copytree(ROOT / "examples/consumer", source, ignore=shutil.ignore_patterns("build"))
            prefixes = [str(prefix)] + [str(path.resolve()) for path in args.dependency_prefix]
            commands = [["cmake", "-S", str(source), "-B", str(workspace / "build"), "-G", "Ninja",
                         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_PREFIX_PATH=" + ";".join(prefixes),
                         "-DFETCHKIT_EXPECTED_VERSION=" + report["version"]],
                        ["cmake", "--build", str(workspace / "build"), "--parallel", "2"]]
            if sys.platform == "win32":
                commands[0] += ["-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++"]
            with (args.output / "consumer-build.log").open("w", encoding="utf-8") as log:
                for command in commands:
                    report["commands"].append(command)
                    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
            content = bytes(index % 251 for index in range(262144))
            # The shared fixture's fixed bytes are checked before running consumers.
            import http_fixture
            if content != http_fixture.CONTENT:
                raise RuntimeError("Consumer and fixture content differ")
            sha = hashlib.sha256(content).hexdigest()
            with RunningFixture() as fixture:
                for name in ("consumer_c", "consumer_cpp"):
                    binary = workspace / "build" / (name + (".exe" if os.name == "nt" else ""))
                    command = [str(binary), str(workspace / (name + "-store")), fixture.url + "/range", sha]
                    result = subprocess.run(command, capture_output=True, text=True, timeout=45, check=True)
                    value = json.loads(result.stdout)
                    if value.get("passed") is not True or value.get("bytes") != len(content):
                        raise RuntimeError("External consumer did not verify all bytes")
                    report["consumers"].append(value)
            report["passed"] = True
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
