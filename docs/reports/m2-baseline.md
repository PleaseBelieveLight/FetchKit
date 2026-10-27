# M2 检查点、校验与发布恢复验证

日期：2026-10-27。M1 已提交为 `5ed1e31 feat: 实现主机单文件下载与安全续传`，M2 位于其后的工作区。
关键源码/构建输入和产物哈希见 [参考记录](m2/reference-build.json)。

结论：主机 M2 已实现持久化检查点、显式重新绑定、可信 SHA-256、Publishing 修复与最小只读租约。
当前执行环境为 Windows；**Linux 与 sanitizer 尚未执行，不能标记 M2 整体验收完成**。
没有系统重启、掉电、真机下载或真实文件系统 ENOSPC 证据；注入和进程终止结果不替代这些环境。

## 工作包与行为

| 工作包 | 实现 | 验证边界 |
|--------|------|----------|
| M2-01 | 单目录所有权、有界 TLV/CRC、原子记录替换、同步与故障屏障 | 纯 C 调用、记录黄金样本、版本/溢出/损坏、同目录进程 Busy；短写/权限/存储错误注入 |
| M2-02 | received/committed 分离、时间/字节检查点、暂停/关闭提交、启动截断 | 数据/记录同步与替换前后 kill；长于/短于已提交前缀、弱/无 ETag 重下 |
| M2-03 | 固定 MIT PicoSHA2、整文件复读、require_digest、两类完整性结果 | 4 个标准向量、真实文件与 >4 GiB 可信摘要；摘要不符不发布 |
| M2-04 | Verifying → Publishing → Completed、启动补全、只读 lease | 暂存/最终文件损坏、缺失、rename/完成提交故障窗口；关闭后 lease 可读并保持目录锁 |
| M2-05 | 确定性子进程屏障、逐点强制终止、扫描隔离、格式文档 | 坏/未来记录逐条隔离，未知可选字段可跳过，隔离身份不被重建；不是 reboot/power-loss 测试 |

实际 API/CLI 用法见 [M2 文档](../m2-storage.md)，字节格式与兼容策略见 [记录 v1](../storage-record-v1.md)。
旧 task token、provider、URL 和 headers 不持久化；宿主必须重新提供凭证并明确允许执行。
M1 数字文件名的孤立 `.part` 不被采用。未知文件仅保留/统计，配额与保留期回收留在 M3。

## 测试证据

| 集合 | 结果与证据 |
|------|------------|
| 主机常规 CTest | 8 组：服务器、C ABI、假会话、存储、协议、恢复、版本、curl 契约；[执行记录](m2/ctest-windows.txt) |
| 协议/CLI | 55 项，现验证 Completed/TransportComplete，保留 M1 的续传、鉴权、TLS、跨 origin、进程锁断言；[协议结果](m2/protocol-windows-pinned.json) |
| 恢复/故障 | 65 项 Windows 检查；覆盖数据写入/同步、半写 `.next`、记录同步/替换、校验、Publishing、rename、目录同步、Completed 与取消 tombstone；[结果](m2/recovery-windows.json) |
| SHA/格式 | 空串、abc、56 字节 padding、100 万个 a；独立 Python 编码黄金样本与 C++ 编解码对照，CRC/未知版本拒绝 |
| 大文件 | 实际 >4 GiB 跨进程检查点、206 续传、SDK DigestVerified、lease 全量读取、独立整文件哈希；[结果](m2/large-file-windows.json) |
| 无 curl | 4 组单元测试；服务器由主机构建单独覆盖，下载 create 保持 NotSupported；[执行记录](m2/ctest-without-curl.txt) |
| curl 契约 | 508 样本通过，传输层源码未改变；[摘要](m2/curl-contract-windows-pinned.json)，原始样本在构建目录 |
| Android | SDK Debug/Release、sample Debug 与单测通过，arm64/x86_64 均编译；Kotlin 仍只有版本 API，未重跑设备测试 |

大文件为 **4,295,032,835 字节**，首次进程在 **4,295,000,064 字节**提交并暂停后退出。
新进程发送 `Range: bytes=4295000064-` / 强 If-Range，完成后 SDK 整文件摘要及独立测试摘要均为
`228f3903446aeb88363ccbf444258f060ac18c5653d978b4bb2f5b04956c757c`。
数据实际经过网络并连续写盘，不使用稀疏文件或模拟计数；临时目录按边界检查后清理。
大文件 Release 配置使用可调的 64 MiB 字节检查点与 1 秒时间阈值；默认 4 MiB 不变，时间/字节触发由常规矩阵另验。
其独立驱动二进制哈希单列；最后的隔离 ID 保护和文件系统异常映射调整由最终常规集合验证，未再次重复大文件数据路径。

## 正确性处理

- 同步失败时旧 committed 保持有效，不能把内存 received 或磁盘长度当作可恢复偏移。
- 安全重下若先截断再更新旧记录，会留下“旧 ETag/偏移、新数据”的退出窗口；现在先提交零偏移，再截断。
- 两次 rename 不能组成事务；用持久 Publishing 记录和启动时重新验证补全，拒绝损坏候选。
- M1 仅检查文件叶节点不足以约束受管子目录；现在拒绝链接目录，隔离异常记录/数据叶节点，并将 POSIX ELOOP 分类为权限错误。
- 已发布对象的取消清理存在竞态风险；进入 Publishing 前串行确认控制请求，此后返回 Busy，校验期间仍可暂停/取消。
- 常规偏移单测曾重复物化 4 GiB 文件，导致与大文件测试竞争 I/O 而超时；改为轻量 seek 边界检查，真实写盘/续传只放入显式大文件集合。

## 依赖与体积

SHA-256 使用 [PicoSHA2 固定提交](https://github.com/okdshin/PicoSHA2/tree/161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29)，
保留原始头文件与 MIT 许可证，CMake 校验文件 SHA-256。CRC 使用平台 zlib：本机开发文件 1.3.2，Android 使用系统 libz；不为摘要新增 TLS 栈。
网络仍为固定 curl 8.20.0 源码参考构建，OpenSSL/运行库由宿主提供，不能称为完整工具链锁定。

arm64-v8a/API24、Release `-Os`、静态 STL、section GC、单导出列表、无 LTO、相同 strip 的独立文件校验器测量：

| 目标 | strip 后共享库字节 |
|------|-------------------|
| 版本 baseline | 208,000 |
| 文件读取与 SHA-256 | 225,256 |
| 差值 | **17,256** |

使用明确的导出列表和直接十六进制格式化，避免保留未使用导出和 stream/locale 格式化路径。
测量包含文件读取、异常与实际链接的静态运行库成本；不是头文件大小，也不是完整 Android 下载成本。
[测量记录](m2/size-arm64.json) 包含命令、NDK/编译器、源码/产物哈希、静态归档及 NEEDED。

普通双 ABI AAR 为 140,409 字节；arm64 JNI 为 208,768 字节，x86_64 为 208,168 字节。
它们增加系统 `libz.so` 动态依赖，但 JNI 仍仅引用版本接口，不能用此 AAR 的大小变化推导完整 M2/Android 接入增量。
Kotlin 运行依赖仍为 stdlib 2.2.10 和 annotations 13.0，见 [依赖树](m2/android-dependencies.txt)。未加入 OkHttp 等实验依赖。

## 复现与未完成验收

本机参考构建：Ninja/g++ 16.1.0、CMake 4.0.1、Python 3.12.0、Windows 11 x64；curl 源码及哈希沿用锁文件。
Android 使用 NDK 28.2.13676358/CMake 3.22.1/JDK 25，工具链和 ABI 未变。
归档入口 `python tools/ci/collect_m2_evidence.py` 只接受已通过的日志和报告，不把缺测项改成通过。

Linux 复现入口：

```sh
bash tools/ci/linux_m2.sh
FETCHKIT_SANITIZERS=address,undefined bash tools/ci/linux_m2.sh
FETCHKIT_SANITIZERS=thread bash tools/ci/linux_m2.sh
FETCHKIT_RUN_LARGE_FILE=1 bash tools/ci/linux_m2.sh
```

提供入口不表示 Linux/ASan/UBSan/TSan 已运行。POSIX 文件链接隔离用例仅在 POSIX 环境启用，Windows 不能代替该验证。
系统重启/掉电、真实小容量文件系统 ENOSPC、Android SHA/恢复运行与完整接入体积仍须对应环境实测。
多任务共享、重试预算、配额/LRU、pin/过期及 remove/trim 留在 M3；本阶段不提前承诺这些能力。
