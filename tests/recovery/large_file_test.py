"""Opt-in real >4 GiB checkpoint/restart/SDK digest/lease validation."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"server"))
from http_fixture import FixtureHandler, RunningFixture
from m1_fixture import LARGE_BLOCK, LARGE_SIZE

DIGEST="228f3903446aeb88363ccbf444258f060ac18c5653d978b4bb2f5b04956c757c"
PREFIX=4*1024**3+32768


class LargeHandler(FixtureHandler):
    def handle_get(self,path,query):
        ranged=self.headers.get("Range")
        start=int(ranged.removeprefix("bytes=").removesuffix("-")) if ranged else 0
        self.send_response(206 if ranged else 200)
        self.send_header("Content-Length",str(LARGE_SIZE-start))
        self.send_header("ETag",'"large-v1"')
        self.send_header("Connection","close")
        if ranged:self.send_header("Content-Range",f"bytes {start}-{LARGE_SIZE-1}/{LARGE_SIZE}")
        self.end_headers()
        end=LARGE_SIZE if ranged else PREFIX
        at=start
        while at<end and not self.server.stopping.is_set():
            count=min(65536-at%65536,end-at)
            self.wfile.write(LARGE_BLOCK[at%65536:at%65536+count]);at+=count
        self.wfile.flush()
        if not ranged:self.wait_connected(60000)
        self.close_connection=True


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--driver",type=Path,required=True)
    parser.add_argument("--work-root",type=Path,required=True)
    parser.add_argument("--report",type=Path,required=True)
    args=parser.parse_args();args.work_root=args.work_root.resolve();args.work_root.mkdir(parents=True,exist_ok=True)
    report={"platform":platform.platform(),"actual_network_and_disk":True,"sparse_simulation":False,"bytes":LARGE_SIZE,"sha256":DIGEST,
            "checkpoint_config_bytes":67108864,"checkpoint_interval_ms":1000,"passed":False}
    started=time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix="m2-large-",dir=args.work_root) as temp,RunningFixture(handler=LargeHandler) as fixture:
            root=Path(temp).resolve()
            assert root.parent==args.work_root
            def run(mode):
                result=subprocess.run([str(args.driver.resolve()),str(root),fixture.url+"/large","large",DIGEST,mode,"67108864","1000","1"],
                                      capture_output=True,text=True,timeout=750)
                assert result.returncode==0,result.stderr
                return json.loads(result.stdout)
            paused=run("large-stage")
            assert paused["snapshot"]["state"]==2 and paused["snapshot"]["committed_offset"]==PREFIX,paused
            report["checkpoint_bytes"]=PREFIX
            done=run("large")
            snapshot=done["snapshot"]
            assert snapshot["state"]==9 and snapshot["integrity"]==2 and snapshot["committed_offset"]==LARGE_SIZE,done
            assert done["acquire"]=="Ok" and done["leased_bytes"]==LARGE_SIZE and done["read_ok"] and done["lock_retained"] and done["lock_released"],done
            assert fixture.server.requests[1]["range"]==f"bytes={PREFIX}-",fixture.server.requests
            obj=root/"objects"/(snapshot["persistent_id"]+".obj")
            digest=hashlib.sha256()
            with obj.open("rb") as stream:
                while block:=stream.read(1024*1024):digest.update(block)
            assert obj.stat().st_size==LARGE_SIZE and digest.hexdigest()==DIGEST
            report["requests"]=fixture.server.requests;report["integrity"]=snapshot["integrity"]
            report["generation"]=snapshot["generation"];report["passed"]=True
    except Exception as error:
        report["failure"]=str(error)
    report["seconds"]=round(time.monotonic()-started,3)
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(report,indent=2));return 0 if report["passed"] else 1


if __name__=="__main__":raise SystemExit(main())
