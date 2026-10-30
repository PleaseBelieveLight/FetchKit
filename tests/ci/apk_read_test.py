"""设备 APK 读取保留原始字节，拒绝失败或超时的读取。"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/packaging"))
import sample_release_startup_test as startup


class ApkReadTest(unittest.TestCase):
    def test_install_diagnostics_keep_only_the_platform_code(self):
        self.assertEqual(startup.adb_failure_code("Failure [INSTALL_FAILED_INVALID_APK: private detail]"), "INSTALL_FAILED_INVALID_APK")
        self.assertEqual(startup.adb_failure_code("Failure [INSTALL_PARSE_FAILED_NO_CERTIFICATES]"), "INSTALL_PARSE_FAILED_NO_CERTIFICATES")
        self.assertEqual(startup.adb_failure_code("private detail"), "Unknown")

    def test_binary_bytes_are_not_decoded_or_translated(self):
        payload = b"PK\x03\x04\xff\x00\r\n\n"
        with tempfile.TemporaryDirectory() as temporary:
            local = Path(temporary) / "installed.apk"
            def run(arguments, stdout, stderr, timeout):
                self.assertEqual(arguments, ["adb", "-s", "emulator-5554", "exec-out", "cat", "/data/app/base.apk"])
                stdout.write(payload)
                return subprocess.CompletedProcess(arguments, 0)
            with patch.object(startup.subprocess, "run", side_effect=run):
                startup.read_installed_apk(["adb", "-s", "emulator-5554"], "/data/app/base.apk", local)
            self.assertEqual(local.read_bytes(), payload)

    def test_diagnostics_identify_service_failure_without_private_details(self):
        result = subprocess.CompletedProcess([], 255, "Performing Push Install\n",
                                             "java.lang.SecurityException: private token\nPermission denied /private/path\n"
                                             "at com.android.server.pm.Service.run(Service.java:12)\n"
                                             "at private.app.User.run(User.java:1)")
        diagnostic = startup.adb_failure_diagnostic(result)
        self.assertEqual(diagnostic["exception_types"], ["java.lang.SecurityException"])
        self.assertEqual(diagnostic["markers"], ["Permission denied", "Performing Push Install"])
        self.assertEqual(diagnostic["framework_methods"], ["com.android.server.pm.Service.run"])
        self.assertNotIn("private", str(diagnostic))

    def test_nonzero_read_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            with patch.object(startup.subprocess, "run", return_value=subprocess.CompletedProcess([], 1)):
                with self.assertRaises(RuntimeError):
                    startup.read_installed_apk(["adb"], "/data/app/base.apk", Path(temporary) / "installed.apk")

    def test_timeout_closes_the_output_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            local = Path(temporary) / "installed.apk"
            with patch.object(startup.subprocess, "run", side_effect=subprocess.TimeoutExpired("adb", 60)):
                with self.assertRaises(subprocess.TimeoutExpired):
                    startup.read_installed_apk(["adb"], "/data/app/base.apk", local)
            local.unlink()


if __name__ == "__main__":
    unittest.main()
