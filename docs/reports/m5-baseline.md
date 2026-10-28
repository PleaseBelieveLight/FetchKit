# M5 本地交付预览

日期：2026-10-27；Apache-2.0 许可与产物补充验证：2026-10-28。版本 `0.1.0`，基准提交 `0f49424` 加本次未提交的 M5 工作区。
这是 alpha 本地预览；没有配置远程上传，没有声明首版整体验收通过。
复现方式见 [交付指南](../m5-release.md)，剩余门槛见 [检查表](../release-checklist.md)。

## 交付与实测

| 范围 | 结果与证据 |
|---|---|
| Native Release 安装 | CMake 精确版本 package、公共头/静态库/CLI/许可目录；固定 curl 8.20.0 |
| 源码树外 C/C++ | 安装前缀重定位后 `find_package`；两个独立宿主各下载/校验/租约读取 262144 字节；[结果](m5/native-consumer.json) |
| AAR 本地 Maven | 双 ABI AAR、sources、POM、module metadata；Kotlin/OkHttp 依赖保留；[发布日志](m5/maven-publish.txt) |
| 源码树外 Android | 只声明 Maven 坐标，API 31 x86_64 的 Release/R8 下载、provider、事件、lease 全文件 SHA 通过；[结果](m5/android-consumer.json) |
| Android 协议 | 74/74，172 个受控请求，包括 JobScheduler 系统停止；[矩阵](m5/android-matrix.json) |
| 有限压力 | seed=20261027，100 轮共享订阅、取消、暂停/恢复、释放重开、账本与完整 lease 内容验证；[结果](m5/stress.json) |
| 进程与系统重启 | SIGKILL / force-stop / 模拟器正常 guest reboot 分别通过，均从 1024 精确 Range/If-Range 恢复并验证摘要；[记录](m5/device-recovery.json) |
| 符号/内容 | 两 ABI 的未剥离库与 AAR ELF Build ID 一致且保留 debug_info；只有 JNI_OnLoad 动态导出；[manifest](m5/package-manifest.json) |

主机完整 CTest **10/10 通过**，450.10 秒，见 [日志](m5/host-ctest.txt)，属于 Windows 真 curl + 测试故障注入构建。
包括 [55 项协议](m5/host-protocol.json)、[21 项调度公共 API](m5/host-scheduler.json)、
[66 项恢复](m5/host-recovery.json) 和 [508 个 curl 契约样本](m5/host-curl-contract.json)。
生产安装包单独使用 Release、关闭故障注入；不能安装测试构建或 sanitizer 构建。
重启前后 guest boot_id 分别为 `6817e429-d065-4021-96b9-f5e7bea1e845`、`9f918d3d-db98-4e8f-b21f-db8618b5f56f`，
没有重启宿主系统，也没有进行物理设备/掉电实验。

## 完整 Android 成本

相同最小 Java 宿主、arm64、min API 24、unsigned Release APK；包含所有新运行依赖，无 Compose/WorkManager。
AGP Release 对应的 native 实际为 RelWithDebInfo `-O2 -g -DNDEBUG`，静态 STL、strip 后 JNI；未使用 M3 独立 `-Os` 数字代替。

| 产物/对照 | 字节 |
|---|---:|
| 双 ABI AAR | 539293 |
| strip 后 arm64 JNI | 550736 |
| strip 后 x86_64 JNI | 568480 |
| bare / production APK，未启用 R8 | 2452 / 1764571 |
| 完整接入 APK 增量，未启用 R8 | 1762119 |
| bare / production APK，启用 R8 | 2328 / 797915 |
| 完整接入 APK 增量，启用 R8 | 795587 |

AAR SHA-256：`f6e2b28caf13251df2260088ff9a6c0422725c293bc386e8457eb73eae5b60e9`。
2026-10-28 加入 Apache-2.0 原文后，AAR 增加 4029 字节，两种 APK 接入增量各增加 4075 字节；JNI 字节未变。
[测量](m5/size-arm64.json) 含构建参数、native profile、产物与输入指纹；[依赖树](m5/android-dependencies.txt)
解析为 OkHttp 5.5.1、Okio 3.18.1、Kotlin 2.2.21 及列出的 AndroidX/annotations。
POM 的声明版本与最终解析版本分开记录，不将未压缩库、AAR ZIP 项和 APK 增量混用。

## 修复原因与处理

| 问题原因 | 修复 |
|---|---|
| 测量宿主 network-security-config 的 domain 未声明 includeSubdomains，Release lint 拒绝构建 | 显式 false，保留仅 loopback 的明文例外 |
| JNI 导出检查将合法 ELF 符号版本后缀 `@@FETCHKIT` 当作额外导出 | 比较规范符号名，保留 readelf 原文及只有 JNI_OnLoad 的门槛 |
| 发布任务执行闭包引用 Gradle script，configuration cache 无法序列化 | 配置阶段捕获普通布尔/集合值；发布配置缓存成功 |
| 外部宿主自动选到缺少 jlink 的 IDE JBR | 指定完整 Adoptium Java toolchain，并提供局部 JAVA_HOME 参数，独立 R8 构建通过 |
| Windows 文本归档使用 CRLF，Git 规范为 LF 后证据索引哈希失配 | 归档与检查报告统一写入 LF，再按归档字节计算索引，核对暂存区文件哈希 |

没有降低 lint、导出、摘要或测试断言。发布器拒绝 probes 和缺少任一 ABI 的配置。

## 尚未满足的发布门槛

Linux（本机无 WSL/Linux 容器）、ASan/UBSan、独立 TSan、真实文件系统 ENOSPC 未执行；
已提供 `linux_m5.sh` 和私有用户/挂载命名空间 tmpfs 驱动，不能将 Windows/故障注入视为这些门槛的通过证据。
本次没有重新执行实际 >4 GiB；M2 Windows 历史证据保留，但不替代 Linux 或目标 Android 实测。
API 24/中间/目标 API、物理 arm64、真实网络切换、物理设备重启与掉电未测。
100 轮是有限压力，不代表长时间运行无泄漏或已通过 sanitizer。

所有者于 2026-10-28 选择 Apache-2.0，已加入根目录 LICENSE、Maven POM、AAR/classes.jar、sources JAR 和 native 安装包，
各产物中的许可证原文与根目录逐字节一致，SHA-256 为 `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`。
补充验证覆盖许可打包、完整接入体积与外部 R8 宿主；上表协议、压力与进程恢复证据仍来自 2026-10-27，未重复执行。
缓存 POM/嵌入 notices 与 NDK NOTICE 已归档，
缺失第三方原文仍须在分发前补齐审核，[清单](m5/dependency-licenses.json) 不代替许可审核。
严格 [发布检查](m5/release-readiness.json) 必须保持 release_ready=false；本地预览验证可以单独通过。
检查器负向验证已确认：篡改 AAR 内容会使预览失败，仅提供 `passed=true` 的无来源证据无法解除 Linux 门槛。
许可补充验证还确认：POM 缺少 Apache-2.0 声明时，打包器拒绝生成预览。
最后候选固定到提交后，应重新构建并归档同批产物、证据与输入 SHA。
