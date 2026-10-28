"""Verify local evidence and block release when required platform evidence is absent."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--preview-only", action="store_true", help="Validate local package/evidence without claiming release acceptance")
    args = parser.parse_args()
    report = {"preview_passed": False, "release_ready": False, "checks": [], "blockers": []}

    def check(name, predicate):
        if not predicate:
            raise RuntimeError(name)
        report["checks"].append(name)

    def load(name):
        return json.loads((args.evidence / name).read_text(encoding="utf-8"))

    try:
        manifest = json.loads((args.preview / "manifest.json").read_text(encoding="utf-8"))
        check("version matches source", manifest["version"] == (ROOT / "VERSION").read_text().strip())
        for name, identity in manifest["artifacts"].items():
            path = (args.preview / name).resolve()
            check("artifact path stays in preview", args.preview.resolve() in path.parents)
            check("artifact fingerprint: " + name, path.is_file() and path.stat().st_size == identity["bytes"] and
                  hashlib.sha256(path.read_bytes()).hexdigest() == identity["sha256"])
        for name, digest in manifest["source_sha256"].items():
            check("source fingerprint: " + name, hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == digest)
        if manifest["project_license_selected"]:
            license_info = manifest.get("project_license", {})
            license_hash = hashlib.sha256((ROOT / "LICENSE").read_bytes()).hexdigest()
            check("Apache-2.0 project license", license_info.get("spdx_id") == "Apache-2.0" and
                  license_info.get("sha256") == license_hash and manifest["source_sha256"].get("LICENSE") == license_hash)
            check("project license included in preview", manifest["artifacts"]["licenses/FetchKit-LICENSE"]["sha256"] == license_hash)
        native, android, matrix, stress, recovery, size = [load(name) for name in
            ("native-consumer.json", "android-consumer.json", "android-matrix.json", "stress.json", "device-recovery.json", "size-arm64.json")]
        check("external C and C++ consumers", native["passed"] and len(native["consumers"]) == 2 and native["relocated"])
        check("external Maven R8/JNI consumer", android["passed"] and android["r8"] and not android["source_tree_dependency"])
        check("Android protocol matrix", matrix["passed"] and len(matrix["cases"]) >= 74 and all(case["passed"] for case in matrix["cases"]))
        check("100 lifecycle stress rounds", stress["passed"] and len(stress["rounds"]) >= 100)
        check("kill/force-stop/reboot restore", recovery["passed"] and {case["kind"] for case in recovery["cases"]} >= {"kill", "force-stop", "reboot"}
              and all(case["passed"] for case in recovery["cases"]) and recovery["boot_id_before"] != recovery["boot_id_after"])
        check("complete production size scope", size["complete_android_download_cost"] and size["profiles"]["production-r8"]["r8"])
        aar = f"maven/com/luofelix/fetchkit/fetchkit/{manifest['version']}/fetchkit-{manifest['version']}.aar"
        check("measured AAR equals packaged AAR", manifest["artifacts"][aar]["sha256"] == size["ordinary_aar"]["sha256"])
        check("external consumer used packaged AAR", android.get("publication_aar_sha256") == manifest["artifacts"][aar]["sha256"])
        report["preview_passed"] = True
        if not manifest["project_license_selected"]:
            report["blockers"].append("Project license is not selected by the owner")
        # Never infer target coverage from Windows or an emulator result. These require separate evidence.
        required = {
            "linux-acceptance.json": "Linux protocol/recovery, real >4 GiB and production consumer acceptance",
            "linux-asan-ubsan.json": "Linux ASan/UBSan acceptance",
            "linux-tsan.json": "Linux TSan acceptance",
            "linux-enospc.json": "Linux real filesystem ENOSPC",
            "android-device-matrix.json": "API 24/intermediate/target and physical arm64 lifecycle/network matrix",
            "android-large-file.json": "Physical Android actual >4 GiB transfer and offset verification",
            "physical-reboot.json": "Target physical-device reboot and storage recovery",
            "license-review.json": "Project and complete resolved third-party license inventory review",
        }
        for name, description in required.items():
            path = args.evidence / name
            if not path.is_file():
                report["blockers"].append(description + " is missing")
                continue
            gate = json.loads(path.read_text())
            matching_sources = gate.get("source_sha256") == manifest["source_sha256"]
            evidence_logs = gate.get("logs", [])
            valid_logs = bool(evidence_logs)
            for item in evidence_logs:
                log = (args.evidence / item.get("file", "")).resolve()
                valid_logs = valid_logs and args.evidence.resolve() in log.parents and log.is_file()
                if valid_logs:
                    valid_logs = hashlib.sha256(log.read_bytes()).hexdigest() == item.get("sha256")
            valid = (gate.get("passed") is True and gate.get("profile") == Path(name).stem and matching_sources
                     and bool(gate.get("commands")) and valid_logs and bool(gate.get("environment")))
            if name.startswith("linux-"):
                valid = valid and gate.get("environment", {}).get("platform") == "Linux"
            if name == "linux-acceptance.json":
                valid = valid and gate.get("actual_large_file_executed") is True
            if name == "android-device-matrix.json":
                valid = valid and gate.get("physical_arm64") is True and {24, 37}.issubset(set(gate.get("apis", [])))
            if name == "android-large-file.json":
                valid = valid and gate.get("physical_arm64") is True and gate.get("actual_large_file_executed") is True
            if name == "physical-reboot.json":
                valid = valid and gate.get("physical_device") is True and gate.get("boot_id_before") and gate.get("boot_id_after") \
                    and gate["boot_id_before"] != gate["boot_id_after"]
            if not valid:
                report["blockers"].append(description + " lacks passing, matching-source, fingerprinted execution evidence")
        report["release_ready"] = not report["blockers"]
    except Exception as error:
        report["error"] = str(error)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({key: report[key] for key in ("preview_passed", "release_ready", "blockers")}))
    return 0 if (report["preview_passed"] if args.preview_only else report["release_ready"]) else 1


if __name__ == "__main__":
    sys.exit(main())
