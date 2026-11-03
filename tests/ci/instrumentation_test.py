"""设备测试完成、失败和用例缺失都必须及时结束等待。"""
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/ci"))
from run_m4_android import instrumentation_finished, instrumentation_passed, read_instrumentation_log


class InstrumentationCompletionTest(unittest.TestCase):
    def test_success_and_unexpected_counts_are_terminal(self):
        for output in ("OK (4 tests)", "OK (3 tests)", "OK (1 test)", "OK (0 tests)"):
            with self.subTest(output=output):
                self.assertTrue(instrumentation_finished(output))

    def test_runner_failure_or_final_code_is_terminal(self):
        for output in ("INSTRUMENTATION_CODE: -1", "INSTRUMENTATION_CODE: 0", "FAILURES!!!", "Process crashed"):
            with self.subTest(output=output):
                self.assertTrue(instrumentation_finished(output))

    def test_progress_is_not_completion(self):
        self.assertFalse(instrumentation_finished("INSTRUMENTATION_STATUS_CODE: 0\nINSTRUMENTATION_STATUS: current=1"))

    def test_all_four_cases_are_required_for_success(self):
        self.assertTrue(instrumentation_passed("\nOK (4 tests)\r\n\r\n"))
        for output in ("OK (3 tests)", "OK (0 tests)", "NOT OK (4 tests)", "OK (4 tests)\nFAILURES!!!"):
            with self.subTest(output=output):
                self.assertFalse(instrumentation_passed(output))

    @patch("run_m4_android.subprocess.run")
    def test_transient_log_timeout_retries_without_losing_result(self, run):
        result = subprocess.CompletedProcess(["adb"], 0, stdout="OK (4 tests)")
        run.side_effect = [subprocess.TimeoutExpired("adb", 20), result]
        self.assertIs(read_instrumentation_log(["adb"], "test.log"), result)
        self.assertEqual(run.call_count, 2)

    @patch("run_m4_android.subprocess.run")
    def test_persistent_log_timeout_fails_after_three_attempts(self, run):
        run.side_effect = subprocess.TimeoutExpired("adb", 20)
        with self.assertRaises(subprocess.TimeoutExpired):
            read_instrumentation_log(["adb"], "test.log")
        self.assertEqual(run.call_count, 3)


if __name__ == "__main__":
    unittest.main()
