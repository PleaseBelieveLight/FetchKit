# 首版发布检查表

当前是本地 `0.1.0` alpha 预览，尚未通过首版发布验收。自动结果见 [M5 报告](reports/m5-baseline.md)。

| 门槛                               | 所需证据                                                | 当前范围                                                        |
|------------------------------------|---------------------------------------------------------|-----------------------------------------------------------------|
| T01～T08 协议、请求边界、取消/重试 | Linux 真后端与 Android API/ABI 矩阵                     | Windows 主机、API 31 x86_64 模拟器                              |
| T01 实际 >4 GiB                    | Linux 与目标物理 Android 传输、内容及 >32 位偏移        | M2 Windows 历史记录；本次未复测大文件                           |
| T09～T12 检查点、损坏、发布        | 逐屏障终止、保守恢复、摘要/租约                         | Windows 故障集合；Android 真实进程恢复                          |
| T13 磁盘与配额                     | 注入短写/错误 + Linux 隔离文件系统真实 ENOSPC           | 注入/逻辑配额已有；Linux 待执行                                 |
| T14～T15 生命周期/并发             | 调度集合、固定 seed、ASan/UBSan 与独立 TSan             | Windows 有限压力；Linux sanitizer 待执行                        |
| T16 Android 后台                   | API 24、中间版本、目标版本与物理 arm64 的停止/切网/备份 | API 31 模拟器，包括系统停止任务；其余待执行                     |
| T17 重启/存储                      | 目标物理设备重启；如承诺掉电则独立掉电实验              | 模拟器 guest 重启单列；物理设备及掉电未测                       |
| T18 Native 接入                    | Release install/export，移到源码树外 C/C++ 实际下载     | Windows 已提供；Linux 待执行                                    |
| T18 AAR                            | 外部 Maven Release/R8 下载、JNI、事件和 lease           | API 31 x86_64 模拟器                                            |
| T18 版本、符号、内容、体积         | VERSION、POM、sources、Build ID、依赖树、SHA manifest   | 本地预览；新候选提交后重建                                      |
| 许可和分发                         | 所有者选择 LICENSE、完整第三方原文审核、坐标所有权/签名 | 项目已采用 Apache-2.0；第三方许可审核与远程渠道待完成，没有上传 |

严格检查需要 `linux-acceptance.json`、`linux-asan-ubsan.json`、`linux-tsan.json`、`linux-enospc.json`、
`android-device-matrix.json`、`android-large-file.json`、`physical-reboot.json`、`license-review.json`
的真实通过证据。 每项记录执行命令、提交/输入 SHA、设备/系统/API/ABI、结果与原始日志路径，不能只修改
passed 标记来解除门槛。 检查器要求 `profile` 等于文件名去除 `.json`，`source_sha256` 与预览 manifest
完全一致，`commands` 非空，
`environment` 记录系统/设备信息，`logs` 为相对证据目录的 `{file, sha256}` 列表且实际文件哈希匹配。
Linux 的 environment.platform 必须为 Linux；大文件必须标记实际执行；Android 矩阵需包含 API 24/37 与
physical_arm64， 物理重启需 physical_device 和变化的前后 boot_id。先完成对应真实执行，再从原始记录生成这些汇总。
设备覆盖不能以 Windows、翻译运行 arm64 或模拟器结果替代。进程退出、force-stop、正常重启与掉电分开表述。

最终分发前确认原始日志无凭证/签名 URL，SDK/Release 不含探针、故障接口和测试信任锚， 未知版本数据被保留、不可用对象不能
acquire、活动任务/lease/pin 保护有效。 库接口保持 alpha 时，在分发说明中要求重新编译宿主，不声明稳定
ABI。
