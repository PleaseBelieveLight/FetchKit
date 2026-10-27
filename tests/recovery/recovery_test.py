"""Deterministic process-kill recovery and storage failure acceptance (not reboot tests)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "server"))
from http_fixture import CONTENT, FixtureHandler, RunningFixture

DIGEST = hashlib.sha256(CONTENT).hexdigest()
ETAG = '"m2-v1"'


class RecoveryHandler(FixtureHandler):
    def handle_get(self, path, query):
        if not path.startswith("/m2/"):
            return super().handle_get(path, query)
        mode = path[4:]
        start = 0
        ranged = self.headers.get("Range") and mode != "ignore"
        if ranged:
            start = int(self.headers["Range"].removeprefix("bytes=").removesuffix("-"))
        body = b"" if mode == "empty" else CONTENT
        if start >= len(body) and ranged:
            return self.send_body(416, b"", [("Content-Range", f"bytes */{len(body)}")])
        self.send_response(206 if ranged else 200)
        self.send_header("Content-Length", str(len(body) - start))
        if mode != "no-etag":
            self.send_header("ETag", 'W/"weak"' if mode == "weak" else ETAG)
        if ranged:
            self.send_header("Content-Range", f"bytes {start}-{len(body)-1}/{len(body)}")
        self.send_header("Connection", "close")
        self.end_headers()
        for at in range(start, len(body), 8192):
            self.wfile.write(body[at:at+8192]); self.wfile.flush()
            if mode == "stall" and not ranged and at >= 8192:
                self.wait_connected(10000)
                break
            if mode in ("slow", "time", "weak", "no-etag"):
                time.sleep(0.04 if mode == "time" else 0.003)
        self.close_connection = True


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def read_record(path):
    raw = path.read_bytes()
    check(raw[:8] == b"FKREC01\0" and len(raw) == struct.unpack_from("<I", raw, 12)[0]+20, "record framing")
    check(zlib.crc32(raw[:16]+raw[20:]) == struct.unpack_from("<I", raw, 16)[0], "record CRC")
    fields = {}
    at = 20
    while at < len(raw):
        tag, flags, length = struct.unpack_from("<HHI", raw, at); at += 8
        fields[tag] = (flags, raw[at:at+length]); at += length
    return fields


def write_record(path, fields, version=1):
    payload = b"".join(struct.pack("<HHI", tag, flags, len(value))+value for tag, (flags, value) in fields.items())
    header = b"FKREC01\0"+struct.pack("<II", version, len(payload))
    path.write_bytes(header+struct.pack("<I", zlib.crc32(header+payload))+payload)


def integer(fields, tag):
    return struct.unpack("<Q", fields[tag][1])[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    args.driver = args.driver.resolve(); args.work_root = args.work_root.resolve()
    args.work_root.mkdir(parents=True, exist_ok=True)
    report = {"platform": platform.platform(), "process_termination_only": True, "reboot_tested": False,
              "power_loss_tested": False, "cases": [], "failures": []}
    with tempfile.TemporaryDirectory(prefix="m2-", dir=args.work_root) as temp:
        workspace = Path(temp).resolve()
        check(workspace.parent == args.work_root, "cleanup boundary")
        with RunningFixture(handler=RecoveryHandler) as fixture:
            def command(root, mode="download", endpoint="normal", digest=DIGEST, required=1, key="resource", size=65536, interval=1000):
                return [str(args.driver), str(root), fixture.url+"/m2/"+endpoint+"?signature=synthetic-secret",
                        key, digest, mode, str(size), str(interval), str(required)]

            def run(root, mode="download", env=None, **options):
                result = subprocess.run(command(root, mode, **options), capture_output=True, text=True, timeout=40, env=env)
                check(result.returncode == 0, f"driver exit {result.returncode}: {result.stderr}")
                check("synthetic-secret" not in result.stdout+result.stderr, "credential diagnostic leak")
                return json.loads(result.stdout)

            def record_path(root):
                records = list((root/"records").glob("*.rec")); check(len(records)==1, "one record expected")
                return records[0]

            def finish(root, **options):
                data = run(root, **options); snap = data["snapshot"]
                check(snap["state"]==9 and snap["published"] and snap["error"]=="Ok", "not Completed")
                check(snap["committed_offset"]==snap["received_bytes"], "final committed prefix")
                check(data["acquire"]=="Ok" and data["lock_retained"] and data["lock_released"] and data["read_ok"], "lease ownership/bytes")
                check(data["leased_bytes"]==snap["received_bytes"], "lease length")
                for path in (root/"records").iterdir():
                    raw=path.read_bytes();check(b"synthetic-secret" not in raw and fixture.url.encode() not in raw and b"Authorization" not in raw, "persisted credentials/URL")
                return snap

            def case(name, function):
                item={"id":name,"passed":False};report["cases"].append(item)
                started=time.monotonic()
                try:
                    detail=function(workspace/name)
                    if detail: item.update(detail)
                    item["passed"]=True
                except Exception as error:
                    item["failure"]=str(error);report["failures"].append(name)
                item["seconds"]=round(time.monotonic()-started,3)
                print(f"{name}: {'passed' if item['passed'] else item['failure']}",flush=True)

            case("digest-verified", lambda root: {"integrity":finish(root)["integrity"]})
            case("transport-complete", lambda root: {"integrity":finish(root,digest="",required=0)["integrity"]})
            case("empty-digest", lambda root: {"bytes":finish(root,endpoint="empty",digest=hashlib.sha256(b"").hexdigest())["received_bytes"]})

            def rejected(root, digest, required, expected):
                data=run(root,digest=digest,required=required)
                check(data.get("enqueue_error")==expected, "constraint not rejected at creation")
                check(not list((root/"records").glob("*.rec")), "invalid request persisted")
            case("require-digest-missing", lambda r: rejected(r,"",1,"InvalidArgument"))
            case("invalid-digest", lambda r: rejected(r,"z"*64,0,"InvalidArgument"))

            def mismatch(root):
                data=run(root,digest="0"*64)
                check(data["snapshot"]["error"]=="IntegrityMismatch" and not data["snapshot"]["published"] and data["acquire"]=="NotFound", "digest mismatch exposed")
                check(not list((root/"objects").glob("*.obj")), "bad content published")
            case("digest-mismatch", mismatch)

            def pause(root, mode="pause", endpoint="stall"):
                data=run(root,mode=mode,endpoint=endpoint)
                snap=data["snapshot"]
                check(snap["state"]==2 and 0<snap["committed_offset"]==snap["received_bytes"], "pause/close did not commit")
                check(data["acquire"] in ("NotFound","Closed"), "paused resource exposed")
                before=len(fixture.server.requests)
                attached=run(root,mode="attach")
                check(attached["snapshot"]["state"]==2 and len(fixture.server.requests)==before, "attach auto-started")
                check(attached["snapshot"]["generation"]==snap["generation"], "attach advanced generation")
                final=finish(root)
                check(final["generation"]>snap["generation"], "restart generation did not advance")
                return {"committed":snap["committed_offset"]}
            case("pause-attach-resume",pause)
            case("close-attach-resume",lambda r:pause(r,"close"))

            def checkpoint_crash(root, point, occurrence=1, endpoint="slow", size=65536, interval=1000, corrupt=False, mode="download"):
                env=os.environ.copy();env.update(FETCHKIT_FAULT_POINT=point,FETCHKIT_FAULT_OCCURRENCE=str(occurrence))
                process=subprocess.Popen(command(root,mode=mode,endpoint=endpoint,size=size,interval=interval),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
                lines=queue.Queue()
                def reader():
                    for line in process.stdout: lines.put(line)
                thread=threading.Thread(target=reader,daemon=True);thread.start()
                try:
                    marker=json.loads(lines.get(timeout=15))
                    check(marker.get("fault_barrier")==point, "did not reach exact fault barrier")
                    process.kill();process.wait(timeout=5)
                finally:
                    if process.poll() is None:process.kill();process.wait(timeout=5)
                    thread.join(timeout=5);process.stdout.close();process.stderr.close()
                path=record_path(root);fields=read_record(path);committed=integer(fields,9)
                files=list((root/"tmp").glob("*.part"))+list((root/"objects").glob("*.obj"))
                check(files and committed<=max(p.stat().st_size for p in files), "record ahead of durable data")
                if corrupt:
                    with files[0].open("r+b") as stream:stream.write(b"corrupt")
                inspected=run(root,mode="inspect")
                state=inspected["records"][0]["state"]
                if mode=="cancel":
                    check(state==5 and not list((root/"tmp").glob("*.part")), "cancel cleanup not repaired")
                    return {"fault":point,"recovered_state":state}
                if corrupt:
                    check(state==4 and inspected["records"][0]["error"]=="IntegrityMismatch", "Publishing corruption trusted")
                    check(run(root,mode="attach")["acquire"]=="NotFound", "Publishing corruption exposed")
                    return {"fault":point,"recovered_state":state}
                check(state in (2,9), "unexpected recovered state")
                snap=finish(root)
                return {"fault":point,"occurrence":occurrence,"committed_before_scan":committed,"recovered_state":state,"integrity":snap["integrity"]}
            for point in ["data_write_before","data_write_after","checkpoint_data_sync_before","checkpoint_data_sync_after",
                          "checkpoint_commit_after","verify_before","verify_after","publishing_commit_after",
                          "object_rename_before","object_rename_after","object_directory_sync_before",
                          "object_directory_sync_after","completed_commit_after"]:
                case("kill-"+point,lambda root,p=point:checkpoint_crash(root,p))
            for point in ["record_write_before","record_partial_write_after","record_sync_before","record_sync_after","record_replace_before",
                          "record_replace_after","record_directory_sync_before","record_directory_sync_after"]:
                case("kill-"+point,lambda root,p=point:checkpoint_crash(root,p,3))
            case("time-checkpoint",lambda r:checkpoint_crash(r,"checkpoint_commit_after",endpoint="time",size=4194304,interval=50))
            case("publishing-part-corrupt",lambda r:checkpoint_crash(r,"publishing_commit_after",corrupt=True))
            case("publishing-object-corrupt",lambda r:checkpoint_crash(r,"object_rename_after",corrupt=True))

            def reset_crash(root):
                run(root,mode="pause",endpoint="stall")
                return checkpoint_crash(root,"reset_commit_after",endpoint="ignore")
            case("kill-reset-before-truncate",reset_crash)
            case("kill-cancel-before-cleanup",lambda r:checkpoint_crash(r,"cancel_commit_after",endpoint="stall",mode="cancel"))

            def mutate_prefix(root, short):
                data=run(root,mode="pause",endpoint="stall");snap=data["snapshot"]
                part=Path(snap["staging_path"])
                if short:
                    with part.open("r+b") as stream:stream.truncate(5)
                else:
                    with part.open("ab") as stream:stream.write(b"uncommitted-tail")
                inspected=run(root,mode="inspect")["records"][0]
                check(inspected["committed_offset"]==(0 if short else snap["committed_offset"]), "prefix repair offset")
                check(part.stat().st_size==inspected["committed_offset"], "tail not truncated")
                finish(root)
            case("longer-than-committed",lambda r:mutate_prefix(r,False))
            case("shorter-than-committed",lambda r:mutate_prefix(r,True))

            def weak(root, endpoint):
                snap=run(root,mode="pause",endpoint=endpoint)["snapshot"]
                before=len(fixture.server.requests);finish(root)
                check(fixture.server.requests[before]["range"] is None, "weak/no ETag resumed")
                return {"discarded_prefix":snap["committed_offset"]}
            case("weak-etag-restart",lambda r:weak(r,"weak"))
            case("missing-etag-restart",lambda r:weak(r,"no-etag"))

            def corruption(root, kind):
                run(root,mode="pause",endpoint="stall");path=record_path(root)
                fields=read_record(path)
                if kind=="crc":
                    raw=bytearray(path.read_bytes());raw[-1]^=1;path.write_bytes(raw)
                elif kind=="future":write_record(path,fields,2)
                elif kind=="truncated":path.write_bytes(path.read_bytes()[:13])
                elif kind=="oversized":path.write_bytes(b"X"*65557)
                elif kind=="overflow":fields[9]=(1,struct.pack("<Q",2**63));write_record(path,fields)
                elif kind=="bad-state":fields[12]=(1,struct.pack("<Q",2**32+2));write_record(path,fields)
                elif kind=="duplicate":
                    raw=path.read_bytes();extra=struct.pack("<HHI",9,1,8)+struct.pack("<Q",0)
                    payload=raw[20:]+extra;head=raw[:12]+struct.pack("<I",len(payload));path.write_bytes(head+struct.pack("<I",zlib.crc32(head+payload))+payload)
                elif kind=="required":fields[900]=(1,b"new");write_record(path,fields)
                elif kind=="utf8":fields[3]=(1,b"\xc0\x80");write_record(path,fields)
                elif kind=="identity":fields[1]=(1,b"f"*64);write_record(path,fields)
                inspected=run(root,mode="inspect")
                check(not inspected["records"] and inspected["quarantined"]==1 and inspected["orphans"]>=1, "bad record not isolated")
                check(inspected["unsupported"]==int(kind in ("future","required")), "unsupported version accounting")
                check(len(list((root/"quarantine").iterdir()))==1, "original bytes not retained")
                # Another resource must remain usable even though this record is bad.
                finish(root,key="healthy")
            for kind in ["crc","future","truncated","oversized","overflow","bad-state","duplicate","required","utf8","identity"]:
                case("record-"+kind,lambda r,k=kind:corruption(r,k))
            def future_without_data(root):
                run(root,mode="pause",endpoint="stall");path=record_path(root)
                write_record(path,read_record(path),2)
                for part in (root/"tmp").glob("*.part"):part.unlink()
                check(run(root,mode="inspect")["unsupported"]==1, "future record not isolated")
                check(run(root).get("enqueue_error")=="Busy", "quarantined future identity overwritten")
                check(not list((root/"records").glob("*.rec")), "future identity recreated")
            case("future-record-without-data",future_without_data)

            def optional(root):
                run(root,mode="pause",endpoint="stall");path=record_path(root);fields=read_record(path)
                fields[900]=(0,b"future-optional");write_record(path,fields)
                inspected=run(root,mode="inspect")
                check(len(inspected["records"])==1 and inspected["quarantined"]==0, "optional field rejected")
                finish(root)
            case("unknown-optional-field",optional)

            def orphan(root):
                root.mkdir();(root/"tmp").mkdir();(root/"objects").mkdir()
                legacy=root/"tmp/1.part";unknown=root/"objects/foreign.bin"
                legacy.write_bytes(b"legacy-untrusted");unknown.write_bytes(b"unknown")
                inspected=run(root,mode="inspect")
                check(inspected["orphans"]==2 and not inspected["records"], "orphans adopted")
                finish(root,key="../untrusted-name")
                check(legacy.read_bytes()==b"legacy-untrusted" and unknown.read_bytes()==b"unknown", "unowned file removed")
            case("orphan-retention-and-path-identity",orphan)

            def constraint_conflict(root):
                run(root,mode="pause",endpoint="stall")
                check(run(root,digest="0"*64).get("enqueue_error")=="InvalidArgument", "constraint overwrite")
                finish(root)
            case("recovered-constraint-conflict",constraint_conflict)

            if os.name!="nt":
                def symlink_record(root):
                    root.mkdir();(root/"records").mkdir()
                    outside=workspace/(root.name+"-outside");outside.write_bytes(b"outside-untouched")
                    link=root/"records"/("a"*64+".rec");link.symlink_to(outside)
                    data=run(root,mode="inspect")
                    check(data["quarantined"]==1 and outside.read_bytes()==b"outside-untouched", "record symlink followed")
                case("record-symlink-isolation",symlink_record)

            def completed_damage(root, same_length=False, missing=False):
                snap=finish(root);obj=root/"objects"/(snap["persistent_id"]+".obj")
                if missing:obj.unlink()
                elif same_length:
                    with obj.open("r+b") as stream:stream.write(b"corrupt")
                else:
                    with obj.open("r+b") as stream:stream.truncate(3)
                data=run(root,mode="attach")
                check(data["snapshot"]["state"]==4 and data["snapshot"]["error"]=="IntegrityMismatch" and data["acquire"]=="NotFound", "damaged Completed exposed")
            case("completed-missing",lambda r:completed_damage(r,missing=True))
            case("completed-length-corrupt",completed_damage)
            case("completed-same-length-corrupt",lambda r:completed_damage(r,same_length=True))

            def fail(root, point, action):
                env=os.environ.copy();env.update(FETCHKIT_FAULT_POINT=point,FETCHKIT_FAULT_ACTION=action)
                if point.startswith("record_"):env["FETCHKIT_FAULT_OCCURRENCE"]="3"
                data=run(root,env=env,endpoint="slow")
                expected={"disk_full":"DiskFull","permission":"PermissionDenied","io":"IoError"}[action]
                check(data["snapshot"]["error"]==expected and data["acquire"]=="NotFound", "storage failure misclassified/published")
                fields=read_record(record_path(root))
                if point in ("checkpoint_data_sync_before","record_sync_before","record_directory_sync_before"):
                    check(integer(fields,9)==0, "failed sync advanced committed")
                if point=="object_rename_before":
                    check(run(root,mode="inspect")["records"][0]["state"]==9, "Publishing failure did not repair")
                return {"error":expected,"committed":integer(fields,9)}
            for point, action in [("data_write_before","disk_full"),("data_write_before","permission"),
                                  ("checkpoint_data_sync_before","io"),("record_sync_before","io"),
                                  ("record_directory_sync_before","io"),("object_rename_before","io")]:
                case("failure-"+point+"-"+action,lambda r,p=point,a=action:fail(r,p,a))
            def short_writes(root):
                env=os.environ.copy();env["FETCHKIT_TEST_SHORT_WRITES"]="1"
                finish(root,env=env)
            case("seven-byte-short-writes",short_writes)

            def cancel(root):
                data=run(root,mode="cancel",endpoint="stall")
                check(data["snapshot"]["state"]==5 and not list((root/"tmp").glob("*.part")), "cancel cleanup")
                check(run(root,mode="inspect")["records"][0]["state"]==5, "cancel resurrected")
            case("cancel-tombstone",cancel)
            def verify_control(root, mode):
                data=run(root,mode=mode)
                check(data["snapshot"]["state"]==(5 if mode=="cancel-verify" else 2) and data["acquire"]=="NotFound", "verification control race")
                if mode=="pause-verify":finish(root)
            case("cancel-during-verification",lambda r:verify_control(r,"cancel-verify"))
            case("pause-during-verification",lambda r:verify_control(r,"pause-verify"))
    report["passed"]=not report["failures"]
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__=="__main__":
    raise SystemExit(main())
