# Android SDK 功能补齐与验收

[文档中心](../README.md) · [核心设计](核心设计与约束.md)

本页记录 Android 下载恢复、状态查询、后台协调与导出适配的专项验收。
当前接口见 [Android API](../reference/AndroidSDK接口与行为.md)，正式发布仍按 [发布验收清单](../release/发布验收清单.md)。

## A1 验收

复用 `tests/unit/download_session_test.cpp`、`tests/unit/scheduler_test.cpp`、协议服务器和
Android `DownloadIntegrationTest.kt`，至少覆盖：

1. 401、403 分别刷新成功；连续拒绝仅刷新一次；无 provider 的失败可观察。
2. 已提交前缀在鉴权失败后仍保留；retry 新链接实际发送精确 Range/If-Range，并通过租约全文件 SHA-256。
3. 原订阅持有旧链接、第二订阅执行 retry，服务器确认本轮使用新链接；首选 provider 取消/释放时无迟到访问。
4. provider 暂时网络失败参与有限重试，AuthRequired 不循环重试；超时、取消和未知异常分类符合契约。
5. 404/410 默认不刷新，显式配置后才刷新；HTTP 200 错误页面无法通过指定可信摘要。
6. 同域/跨域换链、凭证清除、禁止降级；ETag/长度变化安全重下；约束冲突失败后原任务仍可查询和恢复。
7. 重试预算耗尽、Retry-After 等待、重开重绑不绕过限制；新 generation 事件不会覆盖到旧轮次。
8. retry 与 pause/cancel/release/close 的确定性交错；只有一个 writer，停止/关闭后 provider 和网络回调均满足屏障。

## A2 单任务查询

验收覆盖：中文/补充平面身份、非法 ID、NotFound、运行/暂停/失败/完成/过期/Removed、进程重开、无 provider
查询、
query 与 remove/trim/close 的竞态，以及大量任务中单项查询不枚举所有分页。并发删除允许已在线性化点获得的快照或
NotFound，
关闭竞争允许已进入操作完成或 Closed，不通过严格要求单一竞态结果制造脆弱测试。

## A3 后台执行与网络协调

Windows Debug 构建与 5 项示例 JVM 单测通过。API 31 x86_64 首轮下载矩阵通过 84/84，
验证活跃自定义任务不会被新提交覆盖，记录位于 `build/a3-job-admission-20261103/`。
后续 API 31 普通 Job 与 API 35 UIDT 在 x86_64 模拟器各通过 85/85，每轮记录 189 次请求。
系统超时后原 Job 携带 `Range: bytes=1024-` 和 `If-Range` 续传，用户暂停后未再发请求。
结果见 `build/a3-network-intent-20261103/device/report.json` 与
`build/a3-network-api35-20261103/device/report.json`。

验收覆盖：多任务排队不互相覆盖、网络断开/非计费约束变化/Network 替换、用户暂停后切网不误恢复、
Job 停止及重新执行、进程终止后重绑、通知生命周期；分别记录 API 24、中间 API、34+、仓库目标 API 和物理
arm64。
网络断开、非计费条件变化、实际 Network 替换、进程终止后重绑和通知生命周期仍待专项验证。
API 24、目标 API 37 和物理 arm64 仍待覆盖。旧版长期传输适配单独验收。

## A4 文件导出与协程

验收覆盖：零字节与大文件复制、取消/关闭/删除竞争、慢输出和异常输出、多个 collector、重复进入页面、
创建 Future 的完成/取消竞争和慢消费者有界性；示例显示的速度/ETA 继续由宿主按进度采样计算。

2026-11-03 在 Windows 上完成 Debug 构建、可选协程 Release AAR 构建和 22 项 JVM 单测。
API 31 x86_64 模拟器矩阵通过 87/87 项，服务端记录 193 次请求。新增用例覆盖零字节与多块导出、
写入失败、慢输出持有租约时的删除保护、引擎关闭后取消导出，以及暂停恢复时多个 collector 接收相同快照。
原始设备结果在 `build/a4-20261103/rerun/device/report.json`。可选 AAR 为 23,209 字节，
已核对项目许可证和包内无 native 库或协程运行库。真实 SAF 交互与授权故障、超过 4 GiB 的导出、
独立 Release/R8 宿主和完整依赖体积仍待专项验收。

## A5 候选交付检查

命令、环境准备与参数以 [开发指南](../guides/本地构建与测试.md)、[测试说明](../../tests/README.md)
和 [打包指南](../release/SDK打包与发布.md) 为权威位置；本页只列专项检查。

| 改动               | 必需检查                                                                                                                                         |
|--------------------|--------------------------------------------------------------------------------------------------------------------------------------------------|
| A1 核心和 provider | CTest `c_api`、`download_session`、`scheduler_storage`、`download_protocol`、`m3_protocol`；涉及记录提交/恢复时加 `storage`、`download_recovery` |
| A2 查询            | C ABI / C++ 查询用例、存储与调度相关用例、Kotlin/JNI 接入和关闭竞争                                                                              |
| A1/A2 Android      | SDK 构建/单测，正式 Kotlin/JNI/OkHttp 协议驱动；新增场景同步驱动的预期测试数量                                                                   |
| A3                 | sample 构建/单测、对应 instrumentation、系统停止/切网及重开恢复；旧报告不作为新改动通过证据                                                      |
| A4                 | 导出与协程单测/设备用例、生命周期竞争；新增模块做独立依赖、Release/R8 消费和完整接入体积检查                                                     |
| 候选交付           | 受影响的 native 外部消费者、双 ABI AAR、外部 Release/R8 宿主、产物/许可/符号与严格发布检查                                                       |

执行要求：

1. 每次构建、启动模拟器、压力/大文件测试前读取实际可用物理内存；结合所选任务预算保留系统余量。
   资源不足时只回收本次启动且已完成工作的自有进程；仍不足则跳过并报告手动命令，不停止用户或其他任务进程。
   默认顺序执行高负载检查并限制构建并发，避免同时运行 native 构建、Gradle 和新模拟器。
2. 过滤 CTest 使用 `--no-tests=error`；Kotlin/JNI 用例核对实际类/案例数，更新驱动数量门槛而不是删除门槛。
   A1/A2 可复用 `run_m4_android.py`，A3/A4 按实际测试类选择设备入口。
3. 使用新的构建/证据输出路径，不覆盖既有 M0～M5 原始结果；记录提交/输入 SHA、命令、设备/API/ABI 和请求计数。
   服务器用合成签名/凭证，检查跨域去向和日志脱敏。
4. Linux sanitizer 与故障注入隔离构建。Windows、模拟器、SIGKILL、force-stop、重启和掉电分开表述；
   实际 Android >4 GiB 及物理设备门槛仍按发布检查表执行，不因本轮功能完成而豁免。
5. 对外约定变化时同步权威小节及受影响的示例。明确缺测项，按发布清单检查门槛。

## 阶段证据与待验收项

- [x] A0：状态转换、请求来源、预算和生命周期契约固定，主机失败复现通过；[结果与 Android 未执行项](../reports/android-request-recovery-a0.md)。
- [x] A1：新请求重试已实现，主机与 API 31 模拟器验证通过；[结果与剩余平台门槛](../reports/android-request-recovery-a1.md)。
- [x] A2：无订阅可直接查询持久化任务，[主机与 API 31 验证通过](../reports/android-task-query-a2.md)。
- [ ] 第一批交付：A1/A2 对应主机、Android、外部接入及文档检查有当前候选证据，缺测项明确。
- [ ] A3：多任务与网络协调不覆盖用户意图，系统停止后恢复经过明确许可。
- [ ] A4：导出与协程在取消/关闭竞争中无句柄丢失，线程、缓冲和事件保持有界。
- [ ] A5：各批次候选的设备矩阵、打包与体积证据可追溯，发布门槛按真实环境逐项判断。
