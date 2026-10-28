# M5 接入与本地发布预览

版本源是根目录 `VERSION`，当前 `0.1.0`。这是 alpha 接口的本地交付预览，尚未冻结 C ABI；升级库后重新编译宿主。
当前结果与未通过的发布门槛见 [M5 报告](reports/m5-baseline.md)。未配置远程仓库，也没有上传产物。

## Native 安装与独立接入

在具备 C++17、CMake 3.22.1+、Ninja、Python 3.10+、zlib/OpenSSL 开发文件的环境执行：

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFETCHKIT_WITH_CURL=ON -DFETCHKIT_USE_PINNED_CURL=ON -DFETCHKIT_INSTALL=ON
cmake --build build/release --parallel 2
cmake --install build/release --prefix /absolute/path/fetchkit-install
python tools/ci/verify_native_package.py --prefix /absolute/path/fetchkit-install --output build/consumer
```

Windows 增加 `-DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc -DCMAKE_PREFIX_PATH=D:/msys64/ucrt64`；
验证脚本相应增加 `--dependency-prefix D:/msys64/ucrt64`。可用 `FETCHKIT_CURL_ARCHIVE` 指向符合锁文件
SHA-256 的离线 curl 源码包。 安装默认关闭；显式打开后导出公共头文件、静态库、CLI、CMake package
和第三方许可材料，不安装测试或故障驱动。 含 fault injection 或 sanitizer 的构建拒绝安装。Android 正式交付使用
AAR。

独立工程只需：

```cmake
find_package(fetchkit 0.1.0 EXACT CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE fetchkit::core)
```

纯 C 宿主仍须使用 C++ 链接器，见 [完整 C/C++ 示例](../examples/consumer/CMakeLists.txt)
。静态库不表示运行时完全静态： 固定 curl 是随包安装的静态库；OpenSSL、zlib、编译器运行库及系统线程/网络库仍由目标平台提供。
系统 curl 模式通过 `find_dependency(CURL 7.85)` 查找宿主依赖；固定模式要求随安装前缀提供的 curl
8.20.0 CMake 配置。 Linux 发行前记录 `ldd bin/fetchkit_cli`，Windows 记录
`objdump -p bin/fetchkit_cli.exe`，并按目标系统打包其实际动态依赖。 当前 Windows 预览依赖 MSYS2 UCRT64
运行环境，不能称为已验证的 Linux 二进制分发包。

## CLI

`fetchkit_cli --help` 列出全部参数和 shell 命令。`download` 按 namespace=`cli`、key（默认
resource）、version=`1`
复用持久化身份；同一根目录重开时，重新提供 URL/鉴权及相同长度、摘要约束。URL 不参与缓存身份。
`shell` 可 enqueue/attach、查询分页、暂停恢复、取消/释放一个订阅、读取事件、pin/过期/remove/trim/collect、查询账本。
`read ID OFFSET CAPACITY` 仅通过 Completed 对象租约读取，容量 1～65536，返回字节数与十六进制
JSON；不暴露可写对象路径。 shell 输入含空格的字段使用双引号，token 仅在当前进程有效，persistent_id
可在重开后用于资源管理。

| 退出码 | download 含义                                         |
|--------|-------------------------------------------------------|
| 0      | Completed 或帮助/版本                                 |
| 2      | 参数错误                                              |
| 3      | Network / Timeout / CertificateError                  |
| 4      | HttpStatus / AuthRequired                             |
| 5      | ProtocolMismatch / IntegrityMismatch                  |
| 6      | IoError / DiskFull / PermissionDenied / QuotaExceeded |
| 7      | Cancelled                                             |
| 8      | 其他错误（含未配置后端的 NotSupported）               |

shell 命令错误在每条 JSON 的 `status` / `error` 中报告；会话本身可继续，正常 quit 为 0。 进度写
stderr，最终快照写 stdout。快照的 staging_path 仅供诊断，不能作为可用文件；通过租约读取发布对象。

## Maven AAR 与外部 Android 宿主

```powershell
.\gradlew.bat :sdk:publishReleasePublicationToPreviewRepository '-Pfetchkit.probes=false' --max-workers=2
```

产物位于 `build/m5-maven/com/luofelix/fetchkit/fetchkit/0.1.0/`，含双 ABI AAR、sources、POM、Gradle
module metadata 和校验文件。 本地草案坐标是 `com.luofelix.fetchkit:fetchkit:0.1.0`，远程坐标所有权与发布权限尚未配置。
publish 拒绝 probes=true 或缺少 arm64-v8a/x86_64 的配置；普通构建仍可选择单 ABI 做诊断。 POM 声明
Kotlin 标准库和 OkHttp 5.5.1，最终版本以依赖解析结果为准。直接复制 AAR 文件仍须自行声明运行依赖。

可将 [Android 独立示例](../examples/android-consumer/) 复制到仓库外，配置 Android SDK 并使用完整的
Adoptium JDK 25：

```sh
gradle assembleRelease -Pfetchkit.repository=file:///absolute/path/build/m5-maven \
  -Pfetchkit.version=0.1.0 -Pfetchkit.abi=arm64-v8a
```

也可用验证器在临时目录自动构建、安装并运行 R8 后的真实下载：

```powershell
python tools/ci/verify_android_package.py --repository build/m5-maven --sdk D:/Android/Sdk `
  --adb D:/Android/Sdk/platform-tools/adb.exe --serial emulator-5554 --abi x86_64 `
  --java-home C:/path/to/full-jdk-25 --output build/m5-aar-consumer
```

该示例使用 Debug key 签名本地 Release 验证 APK；只为 loopback 放行明文，没有测试 CA，不作为业务应用的发布签名或网络策略。
构建需要完整 JDK（含 jlink），IDE 的裁剪运行时可能缺少该工具。显式 Java toolchain 的依据见
[Android JDK 文档](https://developer.android.com/build/jdks)。 SDK 的线程、Future、provider、事件
executor、Network、备份排除与 JobScheduler 接入规则见 [M4 API](m4-android-api.md)；
存储、鉴权、重试、错误恢复及读取保护见 [M3 API](m3-host-api.md)。不要在 UI 或 provider 中阻塞等待 SDK
Future。

## 符号、依赖与产物一致性

```powershell
python tools/release/package_preview.py --native-prefix build/m5-install --native-build build/m5-release `
  --repository build/m5-maven --dependencies build/m5-size-arm64/sdk-dependencies.txt `
  --ndk D:/Android/Sdk/ndk/28.2.13676358 --output build/m5-preview
```

工具检查两种 ABI、sources/POM、JNI 内容与故障钩子隔离，以 ELF Build ID 找到对应未剥离符号并确认
debug_info。 输出 native 安装包、Maven 目录、符号、依赖树、缓存 POM/嵌入 notices、NDK NOTICE 和逐文件
SHA-256 manifest。 manifest 记录基准提交、工作区是否修改、构建输入指纹和项目许可是否已选择。二进制留在
build，不纳入 Git。 缓存缺失的许可原文仍须在发布前补齐并审核，许可名称元数据不能替代完整许可义务。
项目自身采用所有者于 2026-10-28 选择的 Apache-2.0，原文位于根目录 `LICENSE`。 native 安装和预览包含许可证原文；AAR
的 classes.jar 与 sources JAR 均包含 `META-INF/fetchkit/LICENSE`， Maven POM 同步声明许可名称、官方
URL 和 distribution=repo，manifest 记录 SPDX 标识与 SHA-256。 完整第三方许可原文审核仍为独立发布门槛。

## 故障、压力与发布检查

`tools/ci/linux_m5.sh` 包含原协议/恢复集合、实际安装和外部接入、固定 seed 压力及隔离 tmpfs 的真实
ENOSPC； 另用 `FETCHKIT_SANITIZERS=address,undefined` 和 `FETCHKIT_SANITIZERS=thread` 分别运行。
`FETCHKIT_RUN_LARGE_FILE=1` 开启实际 >4 GiB，`FETCHKIT_STRESS_ROUNDS=1000` 增加有限长稳轮次。 ENOSPC
工具需要 Linux 用户/挂载命名空间；禁用该能力时报告失败，不能用故障注入结果代替。

Android 使用 `run_m4_android.py`、`run_m4_recovery.py`。后者默认分别执行 SIGKILL/force-stop，
`--reboot` 增加选定模拟器 guest 的正常 adb reboot；前后 boot_id 必须变化，重新建立自有 reverse 并验证精确
Range/摘要。 自动 reboot 仅接受 emulator serial，不操作宿主系统；物理设备重启另按测试操作规程记录。
`--large-file` 是独立真实 >4 GiB 任务。以上均不表示覆盖物理设备掉电。

汇总证据见 `docs/reports/m5/`。
`tools/release/check_release.py --preview ... --evidence ... --output ... --preview-only`
验证本地预览；省略 `--preview-only` 为严格发布检查，缺少平台/许可门槛时返回非零。当前发布保持阻断，详情见
[检查表](release-checklist.md)。最终候选需在固定提交重新构建、运行并归档同一批产物。
