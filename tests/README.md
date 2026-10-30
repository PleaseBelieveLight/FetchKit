# FetchKit 主机与平台验证

[文档中心](../docs/README.md) · [构建与运行](../docs/guides/development.md)

## GitHub Actions

日常主机/Android 检查、扩展验证、环境与日志归档以及手动 Alpha 发布见
[持续集成指南](../docs/guides/continuous-integration.md)。CI 工具本身的失败路径检查使用
`python -m unittest discover -s tests/ci -p '*_test.py' -v`；不替代以下运行时验收。
工作流还通过 `source tests/ci/pipeline_test.sh` 在实际 shell 中复现失败命令经过 `tee` 的路径，
确认启用 `pipefail`，防止构建/测试失败被日志管道误报为通过。

## M5 交付验证

完整步骤和发布门槛见 [M5 指南](../docs/release/packaging.md)。`bash tools/ci/linux_m5.sh` 运行主机协议/恢复、
安装导出、源码树外 C/C++、固定 seed 压力和 Linux 隔离 tmpfs 真实 ENOSPC。
Linux sanitizer 分别使用 `FETCHKIT_SANITIZERS=address,undefined` 与 `FETCHKIT_SANITIZERS=thread`；
`FETCHKIT_RUN_LARGE_FILE=1` 执行真实 >4 GiB。缺少 Linux 不能用 Windows 结果替代。
`tools/ci/verify_android_package.py` 在临时独立 Gradle 工程解析本地 Maven AAR，并运行 Release/R8 的下载、事件和租约校验。
验证器同时比较实际 JNI 完整版本与 `VERSION`；主机 `core_version` 也精确校验 CLI 版本，防止预发布后缀丢失。
`run_m4_recovery.py --reboot` 增加模拟器 guest 正常重启、前后 boot_id、精确 Range 与摘要检查；
默认仍为 SIGKILL/force-stop，自动重启仅接受 emulator serial，物理设备与掉电结果单列。
本地证据由 `collect_m5_evidence.py` 归档，`tools/release/check_release.py` 区分预览通过和首版发布通过。

发布配置变更后，运行实际 Gradle 拒绝路径检查（已有依赖缓存时加 `--offline`）：

```powershell
python tests/packaging/publication_guards_test.py --output build/publication-guards --offline
```

覆盖实验探针、缺少 ABI 和缺少认证三种情况；检查跳过产物生成并清空发布凭证，不上传产物。
正常双 ABI 本地发布及外部 Release/R8 接入仍按打包指南单独验证。

## Release 演示 APK 启动回归

构建并验证实际分发的 Release APK，而非 Debug 或测试专用变体：

```powershell
.\gradlew.bat :sample:assembleRelease '-Pfetchkit.probes=false' --max-workers=2 --no-configuration-cache
python tests/packaging/sample_release_startup_test.py --serial <设备序列号> `
  --apk platform/android/sample/build/outputs/apk/release/sample-release.apk --output build/sample-release-startup
```

驱动在明确选中的、已解锁设备上同签名覆盖安装，核对设备 APK 的 SHA-256 和非 debuggable 标志，
执行三次冷启动，检查包含图标与 JNI 版本号的首帧，以及存储、能力指南和新建下载面板。
不启动下载，不卸载或清空应用数据；签名不匹配时失败。已有相同 APK 可加 `--skip-install`，仍核对实际安装内容。
报告记录 API、ABI、设备型号、模拟器标志、检查项和 APK 哈希；失败时归档本应用的崩溃日志，不清空设备日志。
每次运行使用新的 `--output` 路径，保留旧包失败与修复包通过的证据。
API 24 userdebug 测试模拟器若拒绝 shell 读取已安装 APK，可显式增加 `--emulator-root-pull`：
只在读取权限不足时临时 root adbd，读取后立即 unroot，再执行真实 Release 启动验证。
该选项默认关闭，拒绝物理设备；报告单独记录是否使用过临时读取权限。不得跳过设备 APK 的 SHA-256 比对。

## M4 正式 Android SDK

普通 SDK 已使用隔离 OkHttp 5.5.1；设备矩阵测试正式 Kotlin/JNI/C++ 链路，不启用 M0 探针。

```powershell
.\tools\ci\start_m4_emulator.ps1 -SdkRoot D:/Android/Sdk -Avd Small_Phone
python tools/ci/run_m4_android.py --serial emulator-5554
python tools/ci/run_m4_recovery.py --serial emulator-5554
python tools/ci/run_m4_recovery.py --serial <设备序列号> --large-file
```

启动器只使用已存在的 AVD，不停止已有模拟器，不载入/保存快照，并移除子进程继承的代理变量。
设备驱动使用唯一且 `--no-rebind` 的 reverse 规则，避免旧 ADB 多个 tcp:0 允许列表覆盖；只回收自己的映射和日志。
`--device-host` 可显式选取模拟器主机地址作诊断，SDK 不硬编码模拟器地址；不同路由结果分别报告。
每次校验 run_id、完整 JUnit 结果、服务器请求、凭证去向及真实内容，不能只凭 adb 退出码判定通过。

矩阵覆盖 M1/M3 共享场景、真实 TLS、隐藏重试、取消/回调/关闭、pin/过期/lease、部分恢复、UTF-8 身份和请求头。
内存专项实际并发 3×256 KiB 与 3×32 MiB 文件，报告 Android PSS，不能与 Windows RSS 混用。
恢复专项在 committed 屏障后由控制器触发 Process.killProcess 的真实 SIGKILL；另验 am force-stop 后显式重启。
两者都重新创建 provider 并断言精确 Range/If-Range、SDK DigestVerified 和租约全量 SHA-256。
可选大文件入口实际跨过 4 GiB 检查点，至少需要 4.8 GiB 可用空间；默认不运行，不用模拟偏移代替。
API 24/中间/目标 API、arm64 真机、系统停止/网络切换、重启和掉电需要对应记录，见 [M4 报告](../docs/reports/m4-baseline.md)。

## M3 调度、重试、配额与竞态

参考构建开启 `FETCHKIT_BUILD_TESTS`、`FETCHKIT_BUILD_PROBES`、`FETCHKIT_WITH_CURL`、
`FETCHKIT_USE_PINNED_CURL`、`FETCHKIT_ENABLE_FAULT_INJECTION`，执行常规 CTest（排除 `large_file`）。
`scheduler_storage` 含 21 组确定性会话，覆盖 3 活动任务/保留控制通道、共享订阅、provider 生命周期、事件背压与重入、
重试时钟/预算恢复、已知/未知长度配额、LRU/pin/expiry/lease、暂存年龄、旧订阅隔离、排队 generation 和 20 轮并发控制/关闭。
`m3_protocol` 通过公共 C ABI 的 CLI 执行 HTTP 重试/日期、重开等待、共享句柄、存储策略、v1 迁移、分页/索引和进程内存测试。

内存测试同时运行 3 个 256 KiB 文件与 3 个 32 MiB 文件，采样自身子进程 RSS/峰值和线程数；
另外在 128 个任务上记录索引增量。Windows 使用只读进程指标，Linux 使用 /proc，不依赖额外 Python 包。
内存结果是该工具链和请求规模的证据，不能把数据缓冲预算当作总 RSS。
批量关闭包含必要的记录同步；索引专项给 128 个任务较长关闭窗口，不跳过持久化断言。

恢复矩阵增加 Removed tombstone 中断；校验阶段的暂停/取消用协作式确定性屏障，避免轮询碰巧赶在发布之后。
M2 的 >4 GiB 实测保留历史证据；M3 另有 64 位空间预留/回滚单测，不能把它称为新的 >4 GiB 实际传输。
大文件仍可显式开启原测试标签。
Linux 入口为 `tools/ci/linux_m3.sh`，ASan/UBSan 与 TSan 分开配置；缺少环境必须报告。

## M2 恢复与校验

主机配置增加 `-DFETCHKIT_ENABLE_FAULT_INJECTION=ON -DFETCHKIT_BUILD_TESTS=ON`，然后执行
`ctest --test-dir build/m2-host --output-on-failure -LE large_file`。本地 HTTP/HTTPS 测试需要回环套接字许可。
故障注入仅允许主机测试构建；普通 SDK/Android 构建不得开启。恢复驱动在已命名的确定性屏障输出标记，
父进程确认后 kill，再启动全新进程；不能用随机 sleep 代替屏障，也不能称为重启/掉电测试。

`storage` 检查标准 SHA-256 向量、独立编码的记录黄金样本、损坏/版本和 64 位文件偏移；
`download_recovery` 覆盖检查点/记录替换/校验/发布窗口、前缀截断、坏记录隔离、未知字段、摘要失败、
短写/磁盘/同步/权限注入、只读租约及校验期间的控制。原协议集合改为验证 Completed 与 TransportComplete 完整性结果。

开启 `FETCHKIT_BUILD_LARGE_FILE_TESTS=ON` 后，`recovery_large_file` 实际传输 4 GiB+65539 字节，
在 4 GiB+32768 字节提交检查点并结束进程，随后新进程从该偏移续传、SDK 验证可信摘要、通过 lease 读取，
并独立对照整文件哈希。建议 Release/RelWithDebInfo 运行，避免 Debug 摘要计算成本放大；不是稀疏文件或计数模拟。
该大文件配置将字节检查点阈值调为 64 MiB、时间阈值仍为 1 秒；默认 4 MiB 策略不变，阈值触发另由常规恢复矩阵验证。
测试仅在受控临时目录写数据并验证删除边界。存储单元测试的偏移检查与实际网络大文件测试分别报告。

Linux 入口为 `bash tools/ci/linux_m2.sh`，可分别设置 `FETCHKIT_SANITIZERS=address,undefined` 或
`FETCHKIT_SANITIZERS=thread`；大文件另加 `FETCHKIT_RUN_LARGE_FILE=1`。
实测环境与缺测项见 [M2 报告](../docs/reports/m2-baseline.md)，历史用法见 [M2 文档](../docs/archive/m2-storage.md)，
当前接口见 [主机 API](../docs/reference/host-api.md)。

M1 主机验证还包括纯 C ABI、假传输状态机、真实下载协议/HTTPS/CLI 和独立大文件集合。
接口与限制见 [M1 API](../docs/archive/m1-host-api.md)，固定依赖与 Linux
入口见 [依赖说明](../third_party/README.md)。
`ctest --test-dir build/m1-host --output-on-failure -LE large_file` 运行常规集合； 要注册实际 >4 GiB
测试，配置时显式增加 `-DFETCHKIT_BUILD_LARGE_FILE_TESTS=ON`，再执行
`ctest --test-dir build/m1-host --output-on-failure --no-tests=error -L large_file`。
未启用该选项时不会悄悄执行大文件传输；`--no-tests=error` 可避免将空测试集合误报为通过。

测试服务器仅用 Python 3.10+ 标准库。固定响应定义在 `fixtures/m0_scenarios.json`；Linux 和 Android
驱动共用场景 ID、内容规则与断言，驱动本身独立。

## 主机

Linux 安装 C++17 编译器、Ninja、CMake 3.22.1+、Python 3.10+ 和 libcurl 7.85+ 开发包后，在仓库根目录运行：

```sh
cmake -S . -B build/m0-host -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFETCHKIT_BUILD_TESTS=ON -DFETCHKIT_BUILD_PROBES=ON -DFETCHKIT_WITH_CURL=ON
cmake --build build/m0-host --parallel
ctest --test-dir build/m0-host --output-on-failure
```

Windows 可增加 `-DCMAKE_CXX_COMPILER=g++` 和本机 libcurl 安装的 `-DCMAKE_PREFIX_PATH=...`；动态运行库需在
PATH。主机探针并不意味着 Windows 下载成为正式产品承诺。

`unit` 包括服务器单测与版本烟测，`contract` 包括 13 个场景、五类取消各 100 次。原始样本写入构建目录
`reports/curl-contract.json`；测试失败返回非零。FNV-1a 用于比较测试内容，不能代表可信摘要校验。

服务器可单独运行，自动分配端口并输出就绪信息：

```powershell
python tests/server/http_fixture.py --ready-file build/fixture-ready.json
```

只监听回环地址，不记录完整 URL、请求凭证或异常消息。可通过 Ctrl+C 停止。延迟/停顿连接检测客户端关闭，后台线程在服务退出时释放。

## Android

先启动一个已有设备/模拟器，确保 `adb` 在 PATH，然后运行：

Gradle daemon 要求 JDK 25；若本机已安装但未自动发现，可先将当前终端的 `JAVA_HOME` 指向该
JDK，避免工具链重复下载。

```powershell
python tests/contract/run_android_probe.py --serial emulator-5554 --backend platform --repetitions 100 --output build/m0-android
python tests/contract/run_android_probe.py --serial emulator-5554 --backend okhttp --repetitions 100 --output build/m0-okhttp
```

驱动会构建并安装示例及测试 APK，建立独占的临时 adb reverse 规则，运行 instrumentation，提取带 run_id
的报告并回收规则。它保留测试 APK 和 `files/m0-platform.json`，不会卸载其他应用。输出包含 `gradle.log`、
`instrumentation.log` 和 `report.json`；测试进程独占全局 Cookie/Authenticator 实验，不应与其他
instrumentation 并发。

Android 退出码区分不了所有 instrumentation 断言，因此驱动同时检查 JSON gate；旧 run_id
或缺失结果均视为失败。全局凭证策略不满足契约时应失败，不能删除断言使门槛变绿。默认构建和普通设备烟测不包含这些实验源码/依赖。

报告记录每次取消到 worker 与清理线程都停止的耗时，分别统计 p95/max。头屏障、背压由显式 latch
命中；阻塞读取先收到首块再等待 30 ms 后取消。真实 DNS、TLS、弱网、系统停止、检查点和进程恢复仍需后续测试，不能由
M0 推导。
