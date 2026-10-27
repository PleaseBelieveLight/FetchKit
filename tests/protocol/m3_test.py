"""Public C ABI via CLI: retries, shared handles, cache policy and process memory."""
import argparse
import ctypes
from ctypes import wintypes
from email.utils import parsedate_to_datetime
import json
from pathlib import Path
import platform
import queue
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"server"))
from http_fixture import RunningFixture
from m3_fixture import M3Handler

META_BUDGET=2*65556


def check(value,message):
    if not value:raise AssertionError(message)


class Shell:
    def __init__(self,cli,root,extra=(),close_timeout=15):
        self.close_timeout=close_timeout
        self.process=subprocess.Popen([str(cli),"shell","--root",str(root),"--allow-http","--retry-base-ms","10",*extra],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.lines=queue.Queue()
        def reader():
            for line in self.process.stdout:self.lines.put(line)
        self.reader=threading.Thread(target=reader,daemon=True);self.reader.start()
    def command(self,value):
        self.process.stdin.write(value+"\n");self.process.stdin.flush()
        return json.loads(self.lines.get(timeout=10))
    def enqueue(self,key,url):return self.command(f'enqueue "{key}" "{url}"')["accepted"]
    def query(self,task):return self.command(f"query {task}")
    def wait(self,task,states=(9,4,5),timeout=15):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            snapshot=self.query(task)
            if snapshot.get("state") in states:return snapshot
            time.sleep(0.005)
        raise AssertionError("snapshot deadline")
    def close(self):
        try:
            if self.process.poll() is None:
                self.process.stdin.write("quit\n");self.process.stdin.flush();self.process.wait(timeout=self.close_timeout)
        finally:
            if self.process.poll() is None:self.process.kill();self.process.wait(timeout=5)
            self.reader.join(timeout=5)
            self.process.stdin.close();self.process.stdout.close();self.process.stderr.close()
    def __enter__(self):return self
    def __exit__(self,*_args):self.close()


class ProcessMetrics:
    def __init__(self,pid):
        self.pid=pid;self.handle=None
        if sys.platform=="win32":
            self.kernel=ctypes.WinDLL("kernel32",use_last_error=True)
            self.kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD];self.kernel.OpenProcess.restype=wintypes.HANDLE
            self.kernel.CloseHandle.argtypes=[wintypes.HANDLE]
            self.handle=self.kernel.OpenProcess(0x0400|0x0010,False,pid)
            if not self.handle:raise OSError("cannot inspect own child memory")
            class Counters(ctypes.Structure):
                _fields_=[("cb",wintypes.DWORD),("PageFaultCount",wintypes.DWORD),
                    ("PeakWorkingSetSize",ctypes.c_size_t),("WorkingSetSize",ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage",ctypes.c_size_t),("QuotaPagedPoolUsage",ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage",ctypes.c_size_t),("QuotaNonPagedPoolUsage",ctypes.c_size_t),
                    ("PagefileUsage",ctypes.c_size_t),("PeakPagefileUsage",ctypes.c_size_t)]
            self.Counters=Counters
            self.psapi=ctypes.WinDLL("psapi",use_last_error=True)
            self.psapi.GetProcessMemoryInfo.argtypes=[wintypes.HANDLE,ctypes.POINTER(Counters),wintypes.DWORD]
    def sample(self):
        if sys.platform=="win32":
            counters=self.Counters();counters.cb=ctypes.sizeof(counters)
            if not self.psapi.GetProcessMemoryInfo(self.handle,ctypes.byref(counters),counters.cb):raise OSError("memory sample failed")
            return counters.WorkingSetSize,counters.PeakWorkingSetSize
        data=Path(f"/proc/{self.pid}/status").read_text()
        fields={line.split(":",1)[0]:line.split(":",1)[1].strip() for line in data.splitlines() if ":" in line}
        return int(fields["VmRSS"].split()[0])*1024,int(fields["VmHWM"].split()[0])*1024
    def threads(self):
        if sys.platform!="win32":
            return int(next(line.split(":",1)[1] for line in Path(f"/proc/{self.pid}/status").read_text().splitlines() if line.startswith("Threads:")))
        class Entry(ctypes.Structure):
            _fields_=[("dwSize",wintypes.DWORD),("cntUsage",wintypes.DWORD),("th32ThreadID",wintypes.DWORD),
                ("th32OwnerProcessID",wintypes.DWORD),("tpBasePri",wintypes.LONG),("tpDeltaPri",wintypes.LONG),("dwFlags",wintypes.DWORD)]
        self.kernel.CreateToolhelp32Snapshot.argtypes=[wintypes.DWORD,wintypes.DWORD];self.kernel.CreateToolhelp32Snapshot.restype=wintypes.HANDLE
        self.kernel.Thread32First.argtypes=[wintypes.HANDLE,ctypes.POINTER(Entry)];self.kernel.Thread32Next.argtypes=[wintypes.HANDLE,ctypes.POINTER(Entry)]
        snapshot=self.kernel.CreateToolhelp32Snapshot(4,0)
        if snapshot==ctypes.c_void_p(-1).value:raise OSError("thread sample failed")
        try:
            entry=Entry();entry.dwSize=ctypes.sizeof(entry);count=0;valid=self.kernel.Thread32First(snapshot,ctypes.byref(entry))
            while valid:
                if entry.th32OwnerProcessID==self.pid:count+=1
                valid=self.kernel.Thread32Next(snapshot,ctypes.byref(entry))
            return count
        finally:self.kernel.CloseHandle(snapshot)
    def close(self):
        if self.handle:self.kernel.CloseHandle(self.handle);self.handle=None


def main():
    parser=argparse.ArgumentParser();parser.add_argument("--cli",type=Path,required=True)
    parser.add_argument("--work-root",type=Path,required=True);parser.add_argument("--report",type=Path,required=True)
    args=parser.parse_args();args.cli=args.cli.resolve();args.work_root=args.work_root.resolve();args.work_root.mkdir(parents=True,exist_ok=True)
    report={"platform":platform.platform(),"cases":[],"failures":[],"memory":{}}
    with tempfile.TemporaryDirectory(prefix="m3-",dir=args.work_root) as temp,RunningFixture(handler=M3Handler) as fixture:
        workspace=Path(temp).resolve();check(workspace.parent==args.work_root,"temporary boundary")
        def url(mode,key,extra=""):return fixture.url+f"/m3/{mode}?case={key}"+extra
        def case(name,function):
            item={"id":name,"passed":False};report["cases"].append(item);started=time.monotonic()
            try:
                detail=function(workspace/name)
                if detail:item.update(detail)
                item["passed"]=True
            except Exception as error:item["failure"]=str(error);report["failures"].append(name)
            item["seconds"]=round(time.monotonic()-started,3);print(f"{name}: {item.get('failure','passed')}",flush=True)

        def retry_status(root,status):
            key=f"status-{status}"
            with Shell(args.cli,root) as shell:
                task=shell.enqueue(key,url(f"retry-{status}",key))
                snap=shell.wait(task);check(snap["state"]==9 and snap["retry_count"]==1 and snap["request_count"]==2,"HTTP retry failed")
                events=shell.command(f"events {task}")["events"]
                check(sum(e["state"]==9 for e in events)==1 and any(e["state"]==10 for e in events),"retry events/terminal")
                return {"retry_count":snap["retry_count"],"requests":snap["request_count"]}
        for status in [408,429,500,502,503,504]:case(f"http-{status}",lambda r,s=status:retry_status(r,s))

        def no_retry(root,status):
            key=f"no-retry-{status}"
            with Shell(args.cli,root) as shell:
                task=shell.enqueue(key,url(f"retry-{status}",key));snap=shell.wait(task)
                check(snap["state"]==4 and snap["retry_count"]==0 and snap["request_count"]==1,"nonretryable HTTP retried")
        for status in [400,404,501,505]:case(f"no-retry-{status}",lambda r,s=status:no_retry(r,s))

        def retry_after(root,after):
            key="after-"+after
            with Shell(args.cli,root,("--retry-max-ms","20")) as shell:
                task=shell.enqueue(key,url("retry-503",key,"&after="+after));snap=shell.wait(task)
                check(snap["state"]==9 and snap["retry_count"]==1,"Retry-After completion")
            elapsed=fixture.server.m3_timings[key][1]-fixture.server.m3_timings[key][0]
            check(elapsed>=0.9 if after=="1" else elapsed>=0.8,"Retry-After shortened by local cap")
            return {"between_requests_seconds":round(elapsed,3)}
        case("retry-after-seconds",lambda r:retry_after(r,"1"));case("retry-after-date",lambda r:retry_after(r,"date"))

        def restart_wait(root):
            key="restart-wait";address=url("retry-503",key,"&after=120")
            with Shell(args.cli,root) as shell:
                task=shell.enqueue(key,address);before=shell.wait(task,(10,));deadline=before["next_retry_at_ms"]
                check(deadline>=int(time.time()*1000)+118000,"long deadline not retained")
            with Shell(args.cli,root) as shell:
                task=shell.enqueue(key,address);snap=shell.wait(task,(10,))
                check(snap["next_retry_at_ms"]==deadline and snap["retry_count"]==1,"restart budget/deadline changed")
                time.sleep(0.05);check(len(fixture.server.m3_timings[key])==1,"retry after restart too early")
                check(shell.command(f"cancel {task}")["status"]=="Ok","cancel waiting")
                check(shell.wait(task)["state"]==5,"waiting cancel lost")
            return {"persisted_deadline":deadline}
        case("restart-retry-wait-and-cancel",restart_wait)

        def shared(root):
            with Shell(args.cli,root) as shell:
                address=url("stall","shared");a=shell.enqueue("shared",address);b=shell.enqueue("shared",address)
                end=time.monotonic()+5
                while shell.query(b)["received_bytes"]<1024 and time.monotonic()<end:time.sleep(0.005)
                check(shell.query(b)["subscriber_count"]==2,"shared subscriber count")
                check(shell.command(f"cancel {a}")["status"]=="Ok" and shell.query(b)["state"]==1,"cancel affected shared writer")
                check(shell.command(f"pause {b}")["status"]=="Ok" and shell.wait(b,(2,))["committed_offset"]==1024,"shared pause")
                check(shell.command(f"resume {b}")["status"]=="Ok","shared resume")
                snap=shell.wait(b);check(snap["state"]==9 and snap["generation"]==2,"shared completion")
                check(len(fixture.server.m3_timings["shared"])==2,"duplicate shared transfer")
        case("shared-public-handles",shared)

        def cache(root):
            capacity=META_BUDGET*2+2*262144
            with Shell(args.cli,root,("--capacity",str(capacity))) as shell:
                def add(key):
                    task=shell.enqueue(key,url("normal",key));snap=shell.wait(task);check(snap["state"]==9,"cache download");return task,snap["persistent_id"]
                a,first=add("cache-a");b,second=add("cache-b")
                check(shell.command(f'pin "{first}"')["status"]=="Ok","pin")
                c,third=add("cache-c")
                check(shell.query(a)["available"] and not shell.query(b)["available"],"cache pin/LRU")
                check(shell.command("trim 0")["status"]=="QuotaExceeded","trim ignored pin")
                check(shell.command(f'remove "{first}"')["status"]=="Busy","remove ignored pin")
                check(shell.command(f'unpin "{first}"')["status"]=="Ok","unpin")
                check(shell.command(f'expiry "{first}" 1')["status"]=="Ok" and not shell.query(a)["available"],"expiry visibility")
                check(shell.command("trim 0")["status"]=="Ok" and shell.command("stats")["charged_bytes"]==0,"trim metadata refund")
        case("pin-expiry-trim-public-api",cache)

        def unknown(root):
            capacity=META_BUDGET+65536
            with Shell(args.cli,root,("--capacity",str(capacity))) as shell:
                task=shell.enqueue("quota",url("unknown","unknown-quota"));snap=shell.wait(task)
                check(snap["error"]=="QuotaExceeded" and 0<snap["received_bytes"]<=65536 and not snap["published"],"unknown quota")
                check(shell.command("stats")["charged_bytes"]<=capacity,"quota oversold")
        case("unknown-length-quota-public-api",unknown)

        def metadata(root):
            with Shell(args.cli,root) as shell:
                task=shell.enqueue("metadata",url("normal","metadata"));snap=shell.wait(task);key=snap["persistent_id"]
                check(shell.command(f'pin "{key}"')["status"]=="Ok","pin persist setup")
                check(shell.command(f'expiry "{key}" 4102444800000')["status"]=="Ok","expiry persist setup")
                charged=shell.command("stats")["charged_bytes"]
            with Shell(args.cli,root) as shell:
                listed=shell.command("list")["items"][0]
                check(listed["pinned"] and listed["expires_at_ms"]==4102444800000,"policy not persisted")
                check(shell.command("stats")["charged_bytes"]==charged,"restart duplicated ledger")
                check(shell.command(f'remove "{key}"')["status"]=="Busy","restart lost pin")
        case("persistent-pin-expiry-and-ledger",metadata)
        def migration(root):
            raw=(Path(__file__).resolve().parents[1]/"fixtures/record-v1.hex").read_text().strip()
            binary=bytes.fromhex(raw);key=binary[28:92].decode()
            (root/"records").mkdir(parents=True);(root/"tmp").mkdir()
            record=root/"records"/(key+".rec");record.write_bytes(binary)
            (root/"tmp"/(key+".part")).write_bytes(b"x"*17)
            with Shell(args.cli,root) as shell:
                listed=shell.command("list")["items"]
                check(len(listed)==1 and listed[0]["committed_offset"]==17 and listed[0]["state"]==2,"v1 migration lost prefix")
                check(int.from_bytes(record.read_bytes()[8:12],"little")==2,"record not upgraded to v2")
        case("record-v1-migration",migration)

        def index_footprint(root):
            with Shell(args.cli,root,("--active","1"),close_timeout=60) as shell:
                metrics=ProcessMetrics(shell.process.pid)
                try:
                    first=shell.enqueue("index-0",url("stall","index-0"));end=time.monotonic()+5
                    while shell.query(first)["received_bytes"]<1024 and time.monotonic()<end:time.sleep(0.005)
                    warm=metrics.sample()[0]
                    for i in range(1,128):shell.enqueue(f"index-{i}",url("stall",f"index-{i}"))
                    after=metrics.sample()[0]
                    stats=shell.command("stats");check(stats["records"]==128 and stats["charged_bytes"]<=stats["capacity_bytes"],"index admission/ledger")
                    pages=[shell.command(f"list {offset}")["items"] for offset in range(0,128,10)]
                    check(sum(map(len,pages))==128 and len({item["persistent_id"] for page in pages for item in page})==128,"pagination overlap")
                    item={"tasks":128,"warm_tasks":1,"warm_rss":warm,"after_rss":after,"additional_rss_bytes":max(0,after-warm),
                        "bytes_per_additional_task":round(max(0,after-warm)/127,1),"metadata_budget_bytes":stats["metadata_budget_bytes"]}
                    report["index"]=item;return item
                finally:metrics.close()
        case("index-footprint-and-pagination",index_footprint)

        def memory(root,size,name):
            root.mkdir()
            with Shell(args.cli,root) as shell:
                metrics=ProcessMetrics(shell.process.pid);samples=[];thread_counts=[];stopping=threading.Event();errors=[]
                def sample():
                    try:
                        while not stopping.wait(0.01):samples.append(metrics.sample())
                    except Exception as error:errors.append(str(error))
                sampler=threading.Thread(target=sample,daemon=True);sampler.start()
                try:
                    baseline=metrics.sample()[0]
                    tasks=[shell.enqueue(f"memory-{name}-{i}",url("stream",f"memory-{name}-{i}",f"&bytes={size}&slow=1")) for i in range(3)]
                    thread_counts.append(metrics.threads())
                    for task in tasks:check(shell.wait(task,timeout=40)["state"]==9,"memory stream download")
                    samples.append(metrics.sample());thread_counts.append(metrics.threads())
                finally:stopping.set();sampler.join(timeout=5);metrics.close()
                check(not errors and samples,"memory metrics unavailable")
                item={"bytes_per_file":size,"parallel_files":3,"baseline_rss":baseline,"peak_rss":max(item[1] for item in samples),
                      "observed_threads":max(thread_counts),"samples":len(samples)}
                report["memory"][name]=item;return item
        case("memory-small",lambda r:memory(r,262144,"small"))
        case("memory-large",lambda r:memory(r,32*1024*1024,"large"))
        if "small" in report["memory"] and "large" in report["memory"]:
            growth=report["memory"]["large"]["peak_rss"]-report["memory"]["small"]["peak_rss"]
            report["memory"]["peak_growth_bytes"]=growth
            if growth>24*1024*1024:report["failures"].append("memory-grew-with-file-size")
    report["passed"]=not report["failures"];args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__=="__main__":raise SystemExit(main())
