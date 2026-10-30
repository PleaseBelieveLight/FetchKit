"""开机标记不能掩盖系统进程重启；要求连续稳定窗口并保留超时报告。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/ci"))
import wait_android_ready as readiness


class ReadinessTest(unittest.TestCase):
    def run_readiness(self, pid_at, expected_error=False):
        now = [0]
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "ready.json"

            def command(arguments, **kwargs):
                if "pidof" in arguments:
                    text = str(pid_at(now[0]))
                elif "packages" in arguments:
                    text = "package:android\n"
                elif "logcat" in arguments:
                    text = "java.lang.OutOfMemoryError: private details"
                else:
                    text = "1"
                return subprocess.CompletedProcess(arguments, 0, text, "")

            with patch.object(sys, "argv", ["ready", "--serial", "emulator-5554", "--output", str(output)]), \
                    patch.object(readiness.subprocess, "run", side_effect=command), \
                    patch.object(readiness.time, "monotonic", side_effect=lambda: now[0]), \
                    patch.object(readiness.time, "sleep", side_effect=lambda seconds: now.__setitem__(0, now[0] + seconds)), \
                    patch("builtins.print"):
                if expected_error:
                    with self.assertRaises(RuntimeError):
                        readiness.main()
                else:
                    readiness.main()
            return now[0], json.loads(output.read_text())

    def test_restart_resets_the_stable_window(self):
        elapsed, report = self.run_readiness(lambda seconds: 100 if seconds < 10 else 200)
        self.assertEqual(elapsed, 40)
        self.assertTrue(report["passed"])
        self.assertEqual(report["system_server_pids"], ["100", "200"])

    def test_continuous_restart_fails_with_redacted_evidence(self):
        elapsed, report = self.run_readiness(lambda seconds: 100 + seconds, expected_error=True)
        self.assertEqual(elapsed, 180)
        self.assertFalse(report["passed"])
        self.assertEqual(report["platform_crash_diagnostic"]["exception_types"], ["java.lang.OutOfMemoryError"])
        self.assertNotIn("private", str(report))


if __name__ == "__main__":
    unittest.main()
