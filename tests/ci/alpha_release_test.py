"""Alpha/Beta/RC Tag 和已验证产物的关联门槛。"""
from pathlib import Path
import hashlib
import json
import os
import sys
import tempfile
import unittest
from zipfile import ZipFile
from unittest.mock import call, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/release"))
from prepare_alpha import fingerprint, validate_tag, verify_apk_libraries, verify_candidate
import prepare_alpha


class AlphaReleaseTest(unittest.TestCase):
    def test_staging_rejects_tampering_and_hashes_every_asset(self):
        for channel, label in (("alpha", "Alpha"), ("beta", "Beta"), ("rc", "RC")):
            with self.subTest(channel=channel):
                self._check_staging(f"0.2.0-{channel}.3", label)

    def _check_staging(self, version, label):
        # 合成包和报告只用于临时单测目录，不充当设备或发布验收证据。
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "VERSION").write_text(version)
            preview = root / "build/alpha-preview"
            coordinates = preview / f"maven/com/luofelix/fetchkit/fetchkit/{version}"
            coordinates.mkdir(parents=True)
            aar = coordinates / f"fetchkit-{version}.aar"
            apk = root / f"platform/android/sample/build/outputs/apk/release/fetchkit-sample-{version}-release.apk"
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
            manifest = json.loads((assets / "alpha-manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["version"], version)
            self.assertEqual(manifest["commit"], "commit")
            self.assertIs(manifest["release_acceptance"], False)
            notes = (root / "build/alpha-notes.md").read_text(encoding="utf-8")
            self.assertIn(f"FetchKit {version}", notes)
            self.assertIn(f"{label} 预发布，C ABI 尚未冻结", notes)
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

    def test_matching_prerelease_tags_are_accepted(self):
        for version in ("0.0.0-alpha.0", "0.2.0-beta.3", "1.0.0-rc.1"):
            with self.subTest(version=version):
                validate_tag(version, "refs/tags/v" + version, "abc", "abc")

    def test_formal_or_malformed_versions_are_rejected(self):
        for version in ("", "0.2.0", "v0.2.0-beta.3", "0.2-beta.3", "01.2.0-beta.3", "0.02.0-rc.1",
                        "0.2.00-alpha.1", "0.2.0-alpha.01", "0.2.0-beta.01", "0.2.0-rc.01",
                        "0.2.0-preview.1", "0.2.0-Beta.3", "0.2.0-beta", "0.2.0-beta.3+build.1"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                validate_tag(version, "refs/tags/v" + version, "abc", "abc")

    def test_mismatched_tag_or_commit_is_rejected_for_every_channel(self):
        for channel in ("alpha", "beta", "rc"):
            version = f"0.2.0-{channel}.3"
            tag = "refs/tags/v" + version
            for ref, tagged_commit in (("refs/heads/master", "abc"),
                                       (f"refs/tags/v0.2.0-{channel}.4", "abc"),
                                       ("refs/tags/" + version, "abc"),
                                       ("refs/tags/vv" + version, "abc"),
                                       (tag, "def")):
                with self.subTest(channel=channel, ref=ref, tagged_commit=tagged_commit), self.assertRaises(ValueError):
                    validate_tag(version, ref, "abc", tagged_commit)

    def test_invalid_version_is_rejected_before_git_lookup(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for version in ("0.2.0", "v0.2.0-beta.3", "0.2.0-preview.1"):
                (root / "VERSION").write_text(version)
                with self.subTest(version=version), patch.object(prepare_alpha, "ROOT", root), \
                        patch.object(prepare_alpha, "subprocess") as process, \
                        patch.object(sys, "argv", ["prepare_alpha", "--check-tag"]):
                    with self.assertRaises(ValueError):
                        prepare_alpha.main()
                    process.check_output.assert_not_called()

    def test_check_tag_checks_the_exact_commit_without_staging(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for channel in ("alpha", "beta", "rc"):
                version = f"0.2.0-{channel}.3"
                (root / "VERSION").write_text(version)
                with self.subTest(channel=channel), patch.object(prepare_alpha, "ROOT", root), \
                        patch.object(prepare_alpha, "subprocess") as process, \
                        patch.object(sys, "argv", ["prepare_alpha", "--check-tag"]), \
                        patch.dict(os.environ, {"GITHUB_REF": "refs/tags/v" + version}):
                    process.check_output.return_value = "commit"
                    prepare_alpha.main()
                    self.assertEqual(process.check_output.call_args_list, [
                        call(["git", "rev-parse", "HEAD"], cwd=root, text=True),
                        call(["git", "rev-parse", "refs/tags/v" + version + "^{commit}"], cwd=root, text=True)])
                    self.assertFalse((root / "build").exists())

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
