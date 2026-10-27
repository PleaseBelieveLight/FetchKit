"""Isolated arm64 file-verifier cost; never represents complete Android download integration."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
from zipfile import ZipFile

ROOT=Path(__file__).resolve().parents[2]


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--ndk",type=Path,required=True)
    parser.add_argument("--cmake",type=Path,required=True)
    parser.add_argument("--output",type=Path,default=ROOT/"build/m2-size-arm64")
    parser.add_argument("--aar",type=Path,default=ROOT/"platform/android/sdk/build/outputs/aar/sdk-release.aar")
    parser.add_argument("--include-engine",action="store_true",help="Also retain M3 scheduler/storage/C ABI with a fake transport")
    args=parser.parse_args();args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=True)
    extension=".exe" if platform.system()=="Windows" else ""
    prebuilt=next((args.ndk/"toolchains/llvm/prebuilt").iterdir())
    strip=prebuilt/"bin"/("llvm-strip"+extension);readelf=prebuilt/"bin"/("llvm-readelf"+extension)
    cmake=args.cmake.resolve();ninja=cmake.parent/("ninja"+extension)
    configure=[str(cmake),"-S",str(ROOT),"-B",str(args.output),"-G","Ninja",f"-DCMAKE_MAKE_PROGRAM={ninja.as_posix()}",
        f"-DCMAKE_TOOLCHAIN_FILE={(args.ndk/'build/cmake/android.toolchain.cmake').as_posix()}",
        "-DANDROID_ABI=arm64-v8a","-DANDROID_PLATFORM=android-24","-DANDROID_STL=c++_static","-DCMAKE_BUILD_TYPE=Release",
        "-DFETCHKIT_BUILD_CLI=OFF","-DFETCHKIT_BUILD_SIZE_PROBE=ON",
        "-DCMAKE_CXX_FLAGS_RELEASE=-Os -DNDEBUG -ffunction-sections -fdata-sections -fvisibility=hidden -fvisibility-inlines-hidden",
        "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--gc-sections"]
    targets=["baseline","digest"]+(["engine"] if args.include_engine else [])
    build=[str(cmake),"--build",str(args.output),"--target",*["fetchkit_"+mode+"_measure" for mode in targets],"--parallel","2"]
    for name,command in [("configure",configure),("build",build)]:
        with (args.output/(name+".log")).open("w",encoding="utf-8") as log:
            subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    def info(path):
        return {"bytes":path.stat().st_size,"sha256":hashlib.sha256(path.read_bytes()).hexdigest()}
    def needed(path):
        output=subprocess.check_output([str(readelf),"-d",str(path)],text=True)
        return re.findall(r"\(NEEDED\).*?\[(.*?)\]",output)
    report={"platform":platform.platform(),"abi":"arm64-v8a","api":24,"build_type":"Release -Os","lto":False,"strip":"--strip-unneeded",
        "stl":"c++_static (included in binaries)","commands":[configure,build],"complete_android_download_cost":False,
        "base_commit":subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip(),
        "compiler":subprocess.check_output([str(prebuilt/'bin'/('clang++'+extension)),"--version"],text=True).strip(),
        "picosha2":json.loads((ROOT/'third_party/picosha2.lock.json').read_text()),"targets":{}}
    stripped=args.output/"stripped";stripped.mkdir(exist_ok=True)
    for mode in targets:
        original=args.output/("libfetchkit_"+mode+"_measure.so");copy=stripped/original.name
        shutil.copy2(original,copy);subprocess.run([str(strip),"--strip-unneeded",str(copy)],check=True)
        report["targets"][mode]={**info(copy),"needed":needed(copy)}
    report["file_verifier_delta_bytes"]=report["targets"]["digest"]["bytes"]-report["targets"]["baseline"]["bytes"]
    if args.include_engine:
        report["engine_delta_bytes"]=report["targets"]["engine"]["bytes"]-report["targets"]["baseline"]["bytes"]
        report["engine_scope"]="Scheduler/storage/C ABI with fake transport; platform network and JNI download bridge excluded"
    report["core_static_archive"]=info(args.output/"libfetchkit.a")
    if args.aar.exists():
        report["ordinary_aar"]={**info(args.aar),"version_api_only":True,"native":[]}
        with ZipFile(args.aar) as archive:
            for name in archive.namelist():
                if name.endswith(".so"):
                    raw=archive.read(name);path=stripped/Path(name).parent.name
                    path.mkdir(exist_ok=True);copy=path/Path(name).name;copy.write_bytes(raw)
                    report["ordinary_aar"]["native"].append({"path":name,"bytes":len(raw),"sha256":hashlib.sha256(raw).hexdigest(),"needed":needed(copy)})
    sources=subprocess.check_output(["git","ls-files","--cached","--others","--exclude-standard","-z"],cwd=ROOT).decode().split("\0")
    report["source_sha256"]={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in sources if name and
        (name.startswith(("src/","include/","third_party/","tools/size/")) or name=="CMakeLists.txt") and (ROOT/name).is_file()}
    (args.output/"report.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"file_verifier_delta_bytes":report["file_verifier_delta_bytes"],"targets":report["targets"],"ordinary_aar":report.get("ordinary_aar")},indent=2))


if __name__=="__main__":main()
