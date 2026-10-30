"""Alpha Tag 和已验证产物的关联门槛。"""
from pathlib import Path
import hashlib
import json
import os
import sys
import tempfile
import unittest
from zipfile import ZipFile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/release"))
from prepare_alpha import fingerprint, validate_tag, verify_apk_libraries, verify_candidate
import prepare_alpha


class AlphaReleaseTest(unittest.TestCase):
    def test_staging_rejects_tampering_and_hashes_every_asset(self):
        # 合成包和报告只用于临时单测目录，不充当设备或发布验收证据。
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            version = "0.1.0-alpha.1"
            (root / "VERSION").write_text(version)
            preview = root / "build/alpha-preview"
            coordinates = preview / f"maven/com/luofelix/fetchkit/fetchkit/{version}"
            coordinates.mkdir(parents=True)
            aar = coordinates / f"fetchkit-{version}.aar"
            apk = root / "platform/android/sample/build/outputs/apk/release/sample-release.apk"
            apk.parent.mkdir(parents=True)
            for target, prefix in ((aar, "jni"), (apk, "lib")):
                with ZipFile(target, "w") as archive:
                    for abi in ("arm64-v8a", "x86_64"):
                        archive.writestr(f"{prefix}/{abi}/libfetchkit_jni.so", abi.encode())
            for name in (f"fetchkit-{version}-sources.jar", f"fetchkit-{version}.pom", f"fetchkit-{version}.module"):
                (coordinates / name).write_bytes(b"fixture")
            for name in (f"fetchkit-native-{version}.zip", "runtime-dependencies.txt", "symbols/x86_64/debug.so", "licenses/LICENSE"):
                path = preview / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture")
            manifest = {"version": version, "base_commit": "commit", "working_tree_dirty": False,
                        "artifacts": {path.relative_to(preview).as_posix(): fingerprint(path)
                                      for path in preview.rglob("*") if path.is_file()}}
            (preview / "manifest.json").write_text(json.dumps(manifest))
            for name, data in (("release-startup", {"passed": True, "version": version, "apk_sha256": fingerprint(apk)["sha256"]}),
                               ("aar-consumer", {"passed": True, "version": version, "publication_aar_sha256": fingerprint(aar)["sha256"]})):
                path = root / f"build/ci/{name}/report.json"
                path.parent.mkdir(parents=True)
                path.write_text(json.dumps(data))
            with patch.object(prepare_alpha, "ROOT", root), patch.object(prepare_alpha, "subprocess") as process, \
                    patch.object(sys, "argv", ["prepare_alpha", "--stage"]), \
                    patch.dict(os.environ, {"GITHUB_REF": "refs/tags/v" + version}):
                process.check_output.return_value = "commit"
                dependencies = preview / "runtime-dependencies.txt"
                dependencies.write_bytes(b"tampered")
                with self.assertRaises(ValueError):
                    prepare_alpha.main()
                dependencies.write_bytes(b"fixture")
                prepare_alpha.main()
            assets = root / "build/alpha-assets"
            self.assertTrue((assets / f"fetchkit-{version}.module").is_file())
            sums = (assets / "SHA256SUMS").read_text().splitlines()
            self.assertEqual(len(sums), len(list(assets.iterdir())) - 1)
            for line in sums:
                digest, name = line.split("  ")
                self.assertEqual(digest, hashlib.sha256((assets / name).read_bytes()).hexdigest())

    def test_apk_must_contain_the_same_dual_abi_libraries(self):
        with tempfile.TemporaryDirectory() as temporary:
            aar, apk = Path(temporary) / "sdk.aar", Path(temporary) / "sample.apk"
            with ZipFile(aar, "w") as archive:
                for abi in ("arm64-v8a", "x86_64"):
                    archive.writestr(f"jni/{abi}/libfetchkit_jni.so", abi.encode())
            for mode in ("valid", "missing-abi", "changed-library", "test-ca"):
                with ZipFile(apk, "w") as archive:
                    archive.writestr("lib/x86_64/libandroidx.graphics.path.so", b"sample dependency")
                    archive.writestr("lib/x86_64/libfetchkit_jni.so", b"changed" if mode == "changed-library" else b"x86_64")
                    if mode != "missing-abi":
                        archive.writestr("lib/arm64-v8a/libfetchkit_jni.so", b"arm64-v8a")
                    if mode == "test-ca":
                        archive.writestr("res/raw/test.pem", b"test certificate")
                if mode == "valid":
                    verify_apk_libraries(aar, apk)
                else:
                    with self.subTest(mode=mode), self.assertRaises(ValueError):
                        verify_apk_libraries(aar, apk)

    def test_only_matching_alpha_tag_is_accepted(self):
        validate_tag("0.1.0-alpha.1", "refs/tags/v0.1.0-alpha.1", "abc", "abc")
        cases = [("0.1.0", "refs/tags/v0.1.0", "abc", "abc"),
                 ("0.1.0-beta.1", "refs/tags/v0.1.0-beta.1", "abc", "abc"),
                 ("0.1.0-alpha.01", "refs/tags/v0.1.0-alpha.01", "abc", "abc"),
                 ("0.1.0-alpha.1", "refs/heads/master", "abc", "abc"),
                 ("0.1.0-alpha.1", "refs/tags/v0.1.0-alpha.2", "abc", "abc"),
                 ("0.1.0-alpha.1", "refs/tags/v0.1.0-alpha.1", "abc", "def")]
        for case in cases:
            with self.subTest(case=case), self.assertRaises(ValueError):
                validate_tag(*case)

    def test_dirty_stale_or_unverified_artifacts_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            aar, apk = Path(temporary) / "sdk.aar", Path(temporary) / "sample.apk"
            aar.write_bytes(b"aar")
            apk.write_bytes(b"apk")
            preview = {"version": "0.1.0-alpha.1", "base_commit": "abc", "working_tree_dirty": False}
            startup = {"passed": True, "version": preview["version"], "apk_sha256": fingerprint(apk)["sha256"]}
            consumer = {"passed": True, "version": preview["version"], "publication_aar_sha256": fingerprint(aar)["sha256"]}
            def check(p=preview, s=startup, c=consumer):
                verify_candidate(p, s, c, aar, apk, preview["version"], "abc")
            check()
            for changed in ({**preview, "working_tree_dirty": True}, {**preview, "base_commit": "old"}):
                with self.assertRaises(ValueError):
                    check(p=changed)
            for changed in ({**startup, "passed": False}, {**startup, "apk_sha256": "old"}):
                with self.assertRaises(ValueError):
                    check(s=changed)
            with self.assertRaises(ValueError):
                check(c={**consumer, "publication_aar_sha256": "old"})
            apk.write_bytes(b"different apk")
            with self.assertRaises(ValueError):
                check()


if __name__ == "__main__":
    unittest.main()
