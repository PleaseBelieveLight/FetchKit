"""工具链选择与测试结果门槛的失败路径。"""
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/ci"))
from android_ci_config import configuration, emulator_packages, packages
from check_junit_results import check_results


class ConfigurationTest(unittest.TestCase):
    def test_repository_configuration(self):
        self.assertTrue(configuration()["java"].isdigit())

    def test_exact_sdk_selection(self):
        config = {"api": "37", "ndk": "28.2.1", "cmake": "3.22.1"}
        listing = "\n".join(item + " | description" for item in (
            "platforms;android-37.0", "build-tools;37.0.0", "build-tools;37.0.1",
            "build-tools;37.1.0-rc1", "ndk;28.2.1", "cmake;3.22.1", "platform-tools"))
        selected = packages(config, listing)
        self.assertEqual(selected[:2], ["platforms;android-37.0", "build-tools;37.0.1"])
        with self.assertRaises(ValueError):
            packages({**config, "ndk": "99"}, listing)

    def test_platform_and_image_can_have_different_names(self):
        listing = "platforms;android-37.0 | SDK\nsystem-images;android-37;google_apis;x86_64 | image"
        self.assertEqual(emulator_packages(37, listing), {"emulator-platform": "37.0", "emulator-image": "37"})
        with self.assertRaises(ValueError):
            emulator_packages(24, listing)

    def test_old_platform_name(self):
        listing = "platforms;android-24 | SDK\nsystem-images;android-24;google_apis;x86_64 | image"
        self.assertEqual(emulator_packages(24, listing)["emulator-platform"], "24")


class JunitGateTest(unittest.TestCase):
    def test_missing_empty_skipped_and_failed_tests_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for module in ("sdk", "sample"):
                directory = root / module / "testDebugUnitTest"
                directory.mkdir(parents=True)
                (directory / "TEST-example.xml").write_text('<testsuite tests="2" failures="0" errors="0" skipped="0"/>')
            with redirect_stdout(StringIO()):
                self.assertEqual(check_results([root / "sdk", root / "sample"]), 4)
            release = root / "sample/testDebugUnitTest/TEST-example.xml"
            for result in ('<testsuite tests="0"/>', '<testsuite tests="2" skipped="2"/>',
                           '<testsuite tests="2" errors="1"/>', '<testsuite tests="2" failures="1"/>'):
                release.write_text(result)
                with self.subTest(result=result), redirect_stdout(StringIO()), self.assertRaises(ValueError):
                    check_results([root / "sdk", root / "sample"])
            release.unlink()
            with redirect_stdout(StringIO()), self.assertRaises(ValueError):
                check_results([root / "sdk", root / "sample"])


if __name__ == "__main__":
    unittest.main()
