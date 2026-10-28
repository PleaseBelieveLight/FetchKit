"""Seeded, bounded public-API lifecycle stress; does not replace sanitizer/soak runs."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import random
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tests/protocol"), str(ROOT / "tests/server")]
from m3_test import Shell, check
from http_fixture import CONTENT, RunningFixture
from m3_fixture import M3Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=100)
    parser.add_argument("--seed", type=int, default=20261027)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 10000:
        parser.error("rounds must be 1..10000")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    report = {"passed": False, "platform": platform.platform(), "seed": args.seed, "rounds": [],
              "scope": "Public CLI multi-subscription controls and repeated process/engine open-close; bounded finite stress"}
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix="m5-stress-", dir=args.output) as temporary, RunningFixture(handler=M3Handler) as fixture:
            workspace = Path(temporary).resolve()
            check(workspace.parent == args.output, "temporary boundary")
            store = workspace / "store"
            for index in range(args.rounds):
                mode = rng.choice(["complete", "cancel-one", "pause-resume", "release-reopen"])
                key = f"round-{index}"
                url = fixture.url + f"/m3/stall?case={key}"
                with Shell(args.cli.resolve(), store, ("--active", "2", "--capacity", "4194304")) as shell:
                    first = shell.enqueue(key, url)
                    second = shell.enqueue(key, url)
                    check(first != second, "subscription tokens must differ")
                    deadline = time.monotonic() + 15
                    while shell.query(first)["received_bytes"] < 1024:
                        check(time.monotonic() < deadline, "receive deadline")
                        time.sleep(0.005)
                    if mode == "cancel-one":
                        check(shell.command(f"cancel {first}")["status"] == "Ok", "cancel")
                        check(shell.wait(first)["state"] == 5, "revoked subscription state")
                    elif mode == "release-reopen":
                        check(shell.command(f"release {first}")["status"] == "Ok", "release first")
                        check(shell.command(f"release {second}")["status"] == "Ok", "release last")
                    if mode != "release-reopen":
                        check(shell.command(f"pause {second}")["status"] == "Ok", "pause")
                        check(shell.wait(second, states=(2,))["state"] == 2, "durable pause")
                        check(shell.command(f"resume {second}")["status"] == "Ok", "resume")
                        done = shell.wait(second)
                        check(done["state"] == 9 and done["received_bytes"] == len(CONTENT), "completed bytes")
                        persistent_id = done["persistent_id"]
                    stats = shell.command("stats")
                    check(stats["charged_bytes"] <= stats["capacity_bytes"], "quota oversold")
                # Fresh process: no callbacks/handles survive; the new provider is the new URL.
                with Shell(args.cli.resolve(), store, ("--capacity", "4194304")) as reopened:
                    if mode == "release-reopen":
                        task = reopened.enqueue(key, url)
                        done = reopened.wait(task)
                        check(done["state"] == 9, "reopen resume")
                        persistent_id = done["persistent_id"]
                    objects = list((store / "objects").glob(persistent_id + "*"))
                    check(len(objects) == 1, "single published object")
                    check(hashlib.sha256(objects[0].read_bytes()).digest() == hashlib.sha256(CONTENT).digest(), "published content mismatch")
                    actual = bytearray()
                    for offset in range(0, len(CONTENT), 65536):
                        block = reopened.command(f'read "{persistent_id}" {offset} 65536')
                        raw = bytes.fromhex(block["hex"])
                        check(len(raw) == block["bytes"], "lease read length")
                        actual.extend(raw)
                    check(actual == CONTENT, "lease content mismatch")
                    check(reopened.command(f'remove "{persistent_id}"')["status"] == "Ok", "cleanup completed object")
                    check(reopened.command("stats")["records"] == 0, "record leaked")
                report["rounds"].append({"round": index, "mode": mode, "passed": True})
            report["passed"] = True
    finally:
        report["seconds"] = round(time.monotonic() - started, 3)
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "rounds": len(report["rounds"]), "seconds": report["seconds"]}))


if __name__ == "__main__":
    main()
