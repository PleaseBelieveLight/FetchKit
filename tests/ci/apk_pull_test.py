"""旧模拟器 APK 读取失败时保留完整校验，并保证恢复 adbd。"""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/packaging"))
from sample_release_startup_test import AdbCommandError, pull_installed_apk


class ApkPullTest(unittest.TestCase):
    def test_root_failure_also_restores_adbd(self):
        calls = []
        def command(arguments, timeout=30):
            calls.append(arguments[0])
            if arguments[0] == "pull":
                raise AdbCommandError("pull", 1, permission_denied=True)
            if arguments[0] == "root":
                raise AdbCommandError("root", 1)
        with self.assertRaises(AdbCommandError):
            pull_installed_apk(command, "/data/app/base.apk", Path("installed.apk"), True)
        self.assertEqual(calls, ["pull", "root", "unroot", "wait-for-device"])

    def test_permission_fallback_restores_adbd(self):
        for fail_second in (False, True):
            calls = []
            def command(arguments, timeout=30):
                calls.append(arguments[0])
                if arguments[0] == "pull" and (calls.count("pull") == 1 or fail_second):
                    raise AdbCommandError("pull", 1, permission_denied=True)
            if fail_second:
                with self.assertRaises(AdbCommandError):
                    pull_installed_apk(command, "/data/app/base.apk", Path("installed.apk"), True)
            else:
                self.assertTrue(pull_installed_apk(command, "/data/app/base.apk", Path("installed.apk"), True))
            self.assertEqual(calls, ["pull", "root", "wait-for-device", "pull", "unroot", "wait-for-device"])

    def test_no_privilege_change_without_permission_and_explicit_option(self):
        for allow_root, denied in ((False, True), (True, False)):
            calls = []
            def command(arguments, timeout=30):
                calls.append(arguments[0])
                raise AdbCommandError("pull", 1, permission_denied=denied)
            with self.assertRaises(AdbCommandError):
                pull_installed_apk(command, "/data/app/base.apk", Path("installed.apk"), allow_root)
            self.assertEqual(calls, ["pull"])


if __name__ == "__main__":
    unittest.main()
