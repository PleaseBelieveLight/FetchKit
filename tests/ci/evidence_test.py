"""归档保留真实环境、缺失项和日志指纹，不继承正式验收或凭证。"""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/ci"))
import collect_ci_evidence


class EvidenceTest(unittest.TestCase):
    def test_fingerprints_missing_inputs_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs, output = root / "inputs", root / "archive"
            inputs.mkdir()
            (inputs / "result.json").write_text('{"passed": false}', encoding="utf-8")
            (inputs / "private.key").write_text("not an evidence file", encoding="utf-8")
            command = ["collect_ci_evidence", "--profile", "test", "--status", "failure",
                       "--input", str(inputs), str(root / "missing"), "--output", str(output)]
            with patch.object(sys, "argv", command), patch.dict(os.environ, {"GITHUB_TOKEN": "synthetic-secret"}):
                collect_ci_evidence.main()
            raw = (output / "run.json").read_text(encoding="utf-8")
            report = json.loads(raw)
            self.assertFalse(report["release_acceptance"])
            self.assertEqual(report["job_status"], "failure")
            self.assertEqual(len(report["missing_inputs"]), 1)
            self.assertEqual(len(report["logs"]), 1)
            self.assertNotIn("synthetic-secret", raw)
            log = report["logs"][0]
            self.assertEqual(hashlib.sha256((output / log["file"]).read_bytes()).hexdigest(), log["sha256"])
            self.assertEqual((inputs / "result.json").read_bytes(), (output / log["file"]).read_bytes())
            with patch.object(sys, "argv", command), self.assertRaises(FileExistsError):
                collect_ci_evidence.main()


if __name__ == "__main__":
    unittest.main()
