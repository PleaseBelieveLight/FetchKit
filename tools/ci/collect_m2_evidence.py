"""Archive passing M2 reports with current source and artifact fingerprints."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[2]


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--host-build",type=Path,default=ROOT/"build/m2-host")
    parser.add_argument("--large-build",type=Path,default=ROOT/"build/m2-release")
    parser.add_argument("--size-build",type=Path,default=ROOT/"build/m2-size-arm64")
    parser.add_argument("--output",type=Path,default=ROOT/"docs/reports/m2")
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    if platform.system()!="Windows":parser.error("This collector archives the Windows baseline; retain Linux reports under their own platform name.")
    def load(path):return json.loads(path.read_text(encoding="utf-8"))
    def write(name,value):(args.output/name).write_text(json.dumps(value,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    def describe(path):return {"file":str(path.relative_to(ROOT)),"bytes":path.stat().st_size,"sha256":hashlib.sha256(path.read_bytes()).hexdigest()}
    recovery=load(args.host_build/"reports/m2-recovery.json")
    protocol=load(args.host_build/"reports/m1-protocol.json")
    large=load(args.large_build/"reports/m2-large-file.json")
    contract=load(args.host_build/"reports/curl-contract.json")
    size=load(args.size_build/"report.json")
    assert recovery["passed"] and all(case["passed"] for case in recovery["cases"])
    assert len(recovery["cases"])>=65
    assert not protocol["failures"] and all(case["passed"] for case in protocol["cases"])
    assert large["passed"] and large["integrity"]==2 and large["checkpoint_bytes"]>2**32
    assert not contract["failures"] and len(contract["samples"])==508
    for name in ["m2-host-ctest.log","m2-no-curl-ctest.log","m2-large-ctest.log"]:
        assert "100% tests passed" in (ROOT/"build"/name).read_text(encoding="utf-8",errors="replace"),name
    assert "BUILD SUCCESSFUL" in (ROOT/"build/m2-android-final.log").read_text(encoding="utf-8",errors="replace")
    write("recovery-windows.json",recovery);write("protocol-windows-pinned.json",protocol);write("large-file-windows.json",large)
    write("size-arm64.json",size)
    write("curl-contract-windows-pinned.json",{key:value for key,value in contract.items() if key!="samples"} | {"sample_count":len(contract["samples"]),
        "raw_report_sha256":hashlib.sha256((args.host_build/"reports/curl-contract.json").read_bytes()).hexdigest()})
    shutil.copyfile(ROOT/"build/m2-host-ctest.log",args.output/"ctest-windows.txt")
    shutil.copyfile(ROOT/"build/m2-no-curl-ctest.log",args.output/"ctest-without-curl.txt")
    dependencies=(ROOT/"build/m2-android-final.log").read_text(encoding="utf-8",errors="replace")
    start=dependencies.find("releaseRuntimeClasspath -")
    end=dependencies.find("A web-based",start)
    if start>=0: (args.output/"android-dependencies.txt").write_text(dependencies[start:end if end>=0 else None].strip()+"\n",encoding="utf-8")
    names=subprocess.check_output(["git","ls-files","--cached","--others","--exclude-standard","-z"],cwd=ROOT).decode().split("\0")
    sources={}
    for name in names:
        if not name or name.startswith(("docs/reports/",".idea/")):continue
        path=ROOT/name
        if path.is_file() and (path.suffix in (".h",".hpp",".cpp",".c",".py",".sh",".cmake",".kts",".kt",".toml",".hex",".map") or
            path.name in ("CMakeLists.txt","gradle.properties","gradle-wrapper.properties","curl.lock.json","picosha2.lock.json")):
            sources[name]=hashlib.sha256(path.read_bytes()).hexdigest()
    extension=".exe" if platform.system()=="Windows" else ""
    def cmake_profile(build):
        values={}
        for line in (build/"CMakeCache.txt").read_text(encoding="utf-8").splitlines():
            if line.startswith(("//","#")) or "=" not in line or ":" not in line.split("=",1)[0]:continue
            key,value=line.split("=",1);key=key.split(":",1)[0]
            if key.startswith("FETCHKIT_") or key in ("CMAKE_BUILD_TYPE","CMAKE_CXX_COMPILER","CMAKE_CXX_FLAGS","CMAKE_CXX_FLAGS_RELEASE","CMAKE_CXX_FLAGS_DEBUG",
                "CMAKE_PREFIX_PATH","ZLIB_INCLUDE_DIR","ZLIB_LIBRARY_RELEASE","OPENSSL_INCLUDE_DIR","OPENSSL_SSL_LIBRARY","OPENSSL_CRYPTO_LIBRARY"):
                values[key]=value
        return values
    artifacts=[args.host_build/("fetchkit_cli"+extension),args.host_build/"libfetchkit.a",args.large_build/("fetchkit_recovery_driver"+extension),
        ROOT/"platform/android/sdk/build/outputs/aar/sdk-release.aar",args.size_build/"stripped/libfetchkit_baseline_measure.so",
        args.size_build/"stripped/libfetchkit_digest_measure.so"]
    host_profile=cmake_profile(args.host_build)
    zlib_header=(Path(host_profile["ZLIB_INCLUDE_DIR"])/"zlib.h").read_text(encoding="utf-8")
    zlib_version=re.search(r'#define\s+ZLIB_VERSION\s+"([^"]+)"',zlib_header).group(1)
    manifest={"date":"2026-10-27","base_commit":subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip(),
        "platform":platform.platform(),"linux_executed":False,"sanitizers_executed":False,"reboot_tested":False,"power_loss_tested":False,
        "real_filesystem_enospc_tested":False,"device_instrumentation_rerun":False,
        "source_sha256_scope":"Final regular, no-curl, Android and size builds. Large executable fingerprint is separate.",
        "large_file_scope":"Independent Release driver predates final quarantined-ID protection and filesystem exception mapping; final regular tests cover those refinements. Byte I/O, resume and verifier paths are unchanged.",
        "validation":{"host_ctest":"8/8","without_curl_ctest":"4/4 (fixture_server covered in host suite)",
            "protocol_checks":len(protocol["cases"]),"recovery_checks":len(recovery["cases"]),"curl_samples":len(contract["samples"]),
            "actual_large_file":True,"android_build_and_sample_unit_tests":True},
        "backend":contract["backend"],"curl_pin":load(ROOT/"third_party/curl.lock.json"),"picosha2_pin":load(ROOT/"third_party/picosha2.lock.json"),
        "toolchain":{"cmake":subprocess.check_output(["cmake","--version"],text=True).splitlines()[0],
            "cxx":subprocess.check_output(["g++","--version"],text=True).splitlines()[0],
            "python":platform.python_version(),"zlib_headers":zlib_version+" (host); Android system libz"},
        "profiles":{"host":host_profile,"large_file":cmake_profile(args.large_build),"size":size["commands"]},
        "source_sha256":sources,"artifacts":[describe(path) for path in artifacts]}
    write("reference-build.json",manifest)
    print(json.dumps(manifest["validation"],ensure_ascii=False));print("Archived",len(sources),"source/build inputs")


if __name__=="__main__":main()
