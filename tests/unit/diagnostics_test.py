"""验证真实任务边界的日志过滤、关闭与隐私，不混入 stdout 协议。"""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", required=True)
    parser.add_argument("--work-root", required=True)
    args = parser.parse_args()
    work_root = Path(args.work_root).resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    for mode in ("default", "error", "warn", "debug", "switch"):
        with tempfile.TemporaryDirectory(prefix=f"diagnostics-{mode}-", dir=work_root) as temporary:
            assert Path(temporary).resolve().is_relative_to(work_root)
            result = subprocess.run([args.driver, mode, temporary], capture_output=True, encoding="utf-8", timeout=20)
            assert result.returncode == 0, f"{mode}: {result.stderr}"
            assert result.stdout.strip() == '{"passed":true}', f"{mode}: stdout 被日志污染"
            logs = result.stderr
            assert "private-secret" not in logs and "signature=" not in logs and "Authorization" not in logs
            if mode == "default":
                assert logs == "", "SDK 日志默认必须关闭"
            elif mode == "switch":
                assert len(logs.splitlines()) == 1 and "status=Ok" in logs
            else:
                assert logs.count("[ERROR]") == 1
                assert "status=Network" in logs and "platform=917" in logs and "retry=1" in logs
                assert "generation=1" in logs and "persistentId=" in logs
                assert logs.count("[WARN]") == (0 if mode == "error" else 2)
                assert ("[DEBUG]" in logs) == (mode == "debug")
                if mode != "error":
                    assert "quarantined=1" in logs and "unsupported=0" in logs
    print("默认关闭、级别过滤、动态关闭、恢复汇总、重试诊断与脱敏通过")


if __name__ == "__main__":
    main()
