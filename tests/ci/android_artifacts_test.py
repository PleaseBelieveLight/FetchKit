"""版本变更、旧产物残留和缺失产物的定位回归。"""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/ci"))
from android_artifacts import android_artifact


class AndroidArtifactsTest(unittest.TestCase):
    def test_version_change_ignores_existing_old_and_other_variant_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "VERSION").write_text("0.1.0-alpha.1\n")
            for kind in ("aar", "apk"):
                for build_type in ("debug", "release"):
                    path = android_artifact(root, kind, build_type, require_exists=False)
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(b"previous build")
            (root / "VERSION").write_text("0.1.0-alpha.2\n")
            for kind in ("aar", "apk"):
                with self.subTest(kind=kind), self.assertRaises(FileNotFoundError):
                    android_artifact(root, kind)
                current = android_artifact(root, kind, require_exists=False)
                current.write_bytes(b"current release")
                self.assertEqual(android_artifact(root, kind), current)
                self.assertIn("0.1.0-alpha.2-release", current.name)
                with self.assertRaises(FileNotFoundError):
                    android_artifact(root, kind, "debug")

    def test_supported_versions_keep_the_complete_suffix(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for version in ("0.1.0", "0.1.0-alpha.1", "1.2.3-beta.2", "1.2.3-rc.10"):
                (root / "VERSION").write_text(version + "\n")
                with self.subTest(version=version):
                    self.assertEqual(android_artifact(root, "aar", require_exists=False).name,
                                     f"fetchkit-{version}-release.aar")
                    self.assertEqual(android_artifact(root, "apk", "debug", require_exists=False).name,
                                     f"fetchkit-sample-{version}-debug.apk")

    def test_invalid_versions_and_variants_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for version in ("", "01.2.3", "1.2", "1.2.3-alpha.01", "1.2.3-preview.1", "../../other"):
                (root / "VERSION").write_text(version)
                with self.subTest(version=version), self.assertRaises(ValueError):
                    android_artifact(root, "apk", require_exists=False)
            (root / "VERSION").write_text("0.1.0-alpha.1")
            for kind, build_type in (("zip", "release"), ("apk", "../debug")):
                with self.subTest(kind=kind, build_type=build_type), self.assertRaises(ValueError):
                    android_artifact(root, kind, build_type, require_exists=False)


if __name__ == "__main__":
    unittest.main()
