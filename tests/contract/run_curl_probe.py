"""Run the same fixture scenarios used by Android; retain every cancellation sample."""

import argparse
import json
import math
from pathlib import Path
import platform
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "server"))
from http_fixture import CONTENT, SCENARIOS, RunningFixture


def fingerprint(content):
    value = 14695981039346656037
    for byte in content:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{value:016x}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=100)
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 1000:
        parser.error("repetitions must be 1..1000")
    report = {"platform": platform.platform(), "samples": [], "summary": {}, "failures": []}
    with RunningFixture() as fixture:
        for scenario in json.loads(SCENARIOS.read_text())["scenarios"]:
            count = args.repetitions if scenario.get("cancel") else 1
            completed = subprocess.run([
                str(args.probe.resolve()), fixture.url + scenario["path"], scenario["id"], str(count)
            ], capture_output=True, text=True, timeout=60)
            samples = [json.loads(line) for line in completed.stdout.splitlines()]
            report["backend"] = completed.stderr.strip()
            report["samples"].extend(samples)
            if completed.returncode or len(samples) != count:
                report["failures"].append(f'{scenario["id"]}: process/sample count failure')
            for sample in samples:
                errors = []
                if scenario.get("cancel"):
                    completed_first = scenario["id"] == "close-race" and sample["reason"] == "complete" and sample["bytes"] == len(CONTENT)
                    if (sample["reason"] != "cancelled" and not completed_first) or sample["cancel_ms"] > 3000:
                        errors.append("cancellation did not quiesce within 3 seconds")
                else:
                    if sample["reason"] != scenario.get("result", "complete"):
                        errors.append("wrong end reason")
                    if sample["status"] != scenario["status"] or sample["bytes"] != scenario["bytes"]:
                        errors.append("wrong status or byte count")
                    if scenario["id"] in ("reject", "gzip") and sample["body_calls"]:
                        errors.append("body delivered before acceptance")
                    if scenario["id"] == "headers" and sample["duplicate_headers"] != 2:
                        errors.append("duplicate response headers lost")
                    if scenario["id"] in ("bytes", "headers", "range"):
                        expected = CONTENT[1024:] if scenario["id"] == "range" else CONTENT
                        if sample["fingerprint"] != fingerprint(expected):
                            errors.append("content differs")
                if not sample["ready"] or sample["threw"] or sample["header_calls"] > 1:
                    errors.append("callback/barrier failure")
                if errors:
                    report["failures"].append(f'{scenario["id"]}/{sample["iteration"]}: {", ".join(errors)}')
            if scenario.get("cancel") and samples:
                values = sorted(s["cancel_ms"] for s in samples)
                p95 = values[math.ceil(0.95 * len(values)) - 1]
                report["summary"][scenario["id"]] = {"count": len(values), "p95_ms": p95, "max_ms": max(values)}
                if p95 > 1000:
                    report["failures"].append(f'{scenario["id"]}: p95 exceeds 1 second')
            print(f'{scenario["id"]}: {len(samples)} samples', flush=True)
        if any(r["accept_encoding"] != "identity" for r in fixture.server.requests):
            report["failures"].append("request did not use identity encoding")
        report["request_count"] = len(fixture.server.requests)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report["summary"], indent=2))
    if report["failures"]:
        print("\n".join(report["failures"]), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
