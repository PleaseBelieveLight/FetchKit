"""错误 AVD 平台必须失败，不能通过改写 target 来掩盖旧工具兼容问题。"""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/ci"))
from configure_ci_avd import configure


class AvdConfigurationTest(unittest.TestCase):
    def test_valid_target_sets_the_actual_heap_and_preserves_other_properties(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            (home / "fetchkit-ci.ini").write_text("target=android-37.0\n")
            config = home / "fetchkit-ci.avd/config.ini"
            config.parent.mkdir()
            config.write_text("hw.ramSize=4096\nvm.heapSize=228M\ndisk.dataPartition.size=8G\n")
            self.assertTrue(configure(home, "37.0")["passed"])
            self.assertEqual(config.read_text(), "hw.ramSize=4096\ndisk.dataPartition.size=8G\nvm.heapSize=512\n")

    def test_android_zero_or_another_api_is_rejected_without_rewriting_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            metadata = home / "fetchkit-ci.ini"
            for target in ("android-0", "android-31"):
                with self.subTest(target=target):
                    metadata.write_text("target=" + target + "\n")
                    with self.assertRaises(ValueError):
                        configure(home, "37.0")
                    self.assertEqual(metadata.read_text(), "target=" + target + "\n")


if __name__ == "__main__":
    unittest.main()
