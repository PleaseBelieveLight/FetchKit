# M3 调度、重试与资源生命周期验证

[文档中心](../README.md) · [验证报告索引](README.md)

日期：2026-10-27。M2 已提交为 `d90ffca feat: 实现持久化恢复与校验发布`，M3 位于其后的工作区。
源码、构建参数与产物指纹见 [参考记录](m3/reference-build.json)。

结论：M3-01～M3-05 已形成可运行的主机实现，Windows 固定 curl 参考构建、无 curl 单元测试与 Android
编译回归通过。 **Linux/sanitizer 和真实小容量文件系统 ENOSPC 尚未验证，M3 不标记为整体验收通过。**
Android 下载桥接与设备生命周期仍属于 M4；本阶段未进行系统重启、掉电或设备专项。

## 工作包与实现

| 工作包 | 结果                                                                                                           |
|--------|----------------------------------------------------------------------------------------------------------------|
| M3-01  | 有界 FIFO、默认 3 个网络 worker 与 1 个控制 worker；同身份单写入者、独立订阅 token、generation、分页与紧凑事件 |
| M3-02  | 独立取消与共享 pause/resume、最后订阅退出保留检查点；provider 解除屏障、关闭停写屏障、事件背压与重入           |
| M3-03  | 分类重试、注入 Clock/随机种子、指数退避和正向抖动、Retry-After 秒数与三类 HTTP-date；预算/截止时间落盘         |
| M3-04  | 已知长度提前预留、未知长度 64 KiB 分块申请；暂存/对象/隔离文件/元数据统一逻辑账本、失败回滚与启动重算          |
| M3-05  | 读取租约保护、pin_on_create、动态 pin/过期、持久访问序号 LRU、remove tombstone、trim 与暂存保留期回收          |

接口与边界见 [M3 API](../reference/host-api.md)
，持久格式见 [v2 记录](../reference/storage-record-v2.md)。 原始 v1 黄金样本不变，新增独立编码 v2
黄金样本；v1 可迁移，未来版本/未知必须字段继续隔离并保护身份。 C ABI 增加字段后须重新编译 alpha 客户端。

两项实施细化已同步设计与计划：

- 宿主在自己的 executor 调用同步 `fetchkit_task_dispatch`，每订阅串行、回调无内部锁；核心不向不可控
  executor 投递闭包。 每订阅最多 64 个紧凑事件，约每 200 ms 合并进度，溢出明确终止该订阅并保留
  SubscriptionOverflow 通知。
- 配额、删除、pin 与租约必须共享记录事务及保护锁，因此缓存策略归入 `src/storage/storage_policy.cpp`
  ，没有新增独立 cache 目标。

## 测试证据

| 集合                 | 结果与证据                                                                                                                                               |
|----------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------|
| 固定 curl 常规 CTest | 10/10；含服务器、纯 C ABI、会话、存储、调度、旧协议、新 API、恢复、版本、curl 契约；[日志](m3/ctest-windows.txt)                                         |
| 调度/存储            | 21 组；含 20 轮 cancel/pause/release/close 并发、慢回调关闭、provider 切换、旧订阅隔离、排队 generation 与单调时钟跳变；[输出](m3/scheduler-windows.txt) |
| 原协议/CLI           | 55 项，保留 Range/206/416、鉴权/TLS/跨 origin、进程锁与内容断言；[结果](m3/protocol-windows-pinned.json)                                                 |
| M3 公共 API          | 21 项，真实 HTTP 重试/等待恢复、共享句柄、存储策略、v1 迁移、分页、内存与索引；[结果](m3/m3-public-api-windows.json)                                     |
| 恢复/故障            | 66 项 Windows 检查，新增 Removed tombstone 中断修复；校验取消/暂停使用确定性协作屏障；[结果](m3/recovery-windows.json)                                   |
| curl 契约            | 508 样本，传输策略契约保持通过；[摘要](m3/curl-contract-windows-pinned.json)                                                                             |
| 无 curl              | 5/5，服务器已由参考集合覆盖；[日志](m3/ctest-without-curl.txt)                                                                                           |
| Android              | SDK Debug/Release、sample Debug 与单测通过；arm64-v8a/x86_64 编译，未重跑设备测试；[运行依赖](m3/android-dependencies.txt)                               |

每 engine 上限为 1024 未释放订阅，每任务上限为 64，读取租约上限为每存储上下文 1024。
慢消费者和已取消订阅压力验证了事件容量与每任务订阅边界。 限额包括已取消但尚未 release
的订阅，不能靠反复取消绕过内存边界。 运行中添加订阅不改变 writer 的 provider、取消源或发布门闩；解除
provider 后不再使用对应用户指针。

## 内存、线程与索引

公共 API 测试在独立 CLI 子进程内采样 Windows Working Set/峰值及线程数；Linux 驱动提供 /proc
采样但本次没有执行。 同时下载 3 个 256 KiB 文件与 3 个 32 MiB 文件，对比峰值变化；文件真实经过网络、写盘和发布。
最终结果如下，原始采样计数见 [M3 公共 API 报告](m3/m3-public-api-windows.json) 的 `memory` / `index`。

| 指标                              | 结果                                 |
|-----------------------------------|--------------------------------------|
| 3×256 KiB 文件峰值 Working Set    | 12161024 字节                        |
| 3×32 MiB 文件峰值 Working Set     | 12234752 字节                        |
| 大小文件峰值差                    | 73728 字节                           |
| 两组观察到的最大总线程数          | 9                                    |
| 1 → 128 个任务的 Working Set 增量 | 1642496 字节，约 12933 字节/新增任务 |

这是指定工具链/后端/样本规模的进程观测，包含运行库、网络与工作区，不能把流式块预算当作总 RSS。
工作线程上限为活动配置加一个控制线程；后端解析线程另计。索引测试记录 1 个预热任务到 128 个任务的 RSS
增量， 元数据账本预算为 128×131112 = 16782336 字节；该磁盘预算不是每任务内存。 128 个任务关闭包含记录同步，专项允许
60 秒退出窗口，未跳过持久化或重开断言。

64 位配额测试验证 >4 GiB 预留与回滚算术；它不是实际 >4 GiB 下载。
[M2 大文件结果](m2/large-file-windows.json) 为此前版本的历史证据，本次未重复该实际传输，也不将其改标为
M3 结果。

## 问题原因与修复

- 共享任务创建路径曾重设正在运行的取消源/provider/发布标记；现在只在静止任务准备新运行时修改，活动订阅只建立独立绑定。
  provider 逐订阅计数和协作取消，release 等待已进入调用结束；切换绑定仍遵守 origin 凭证隔离。
- 最后订阅的自动暂停曾覆盖显式取消；控制优先级现在保留 Cancel，保留控制 worker 避免排队取消被停顿网络饿死。
  清理后的旧订阅以当前 writer 身份和记录存在性校验，不能恢复缺失前缀或控制同身份的新任务。
- 创建失败时全量重算会丢失其他活动任务的预留；现在仅回滚当前申请。入队在写记录前建立临时保护，读取/删除/淘汰共享保护锁。
  租约先关闭文件后释放计数，避免 Windows 删除窗口；零计数检查不再向索引插入空节点。
- 扫描修复曾刷新暂存年龄，反复重开可能无限延长保留；现在扫描保留时间。暂停前尚未启动 attempt 的新
  generation/鉴权预算也明确提交。 UTC 前跳曾可能缩短进程内等待；现在同时检查单调截止时间，重开仍以持久
  UTC 重建等待。
- 已捕获存储上下文的调用可能在 engine close 后继续变更文件；关闭现在等待入队事务、停止 worker
  并建立存储停写屏障，既有 lease 仍可读。
- 校验控制测试的状态轮询可能跨过 Publishing 门闩；改用只在测试构建启用的协作屏障命中校验窗口，保留取消/暂停原断言。

## 依赖与体积

没有新增第三方依赖，curl 8.20.0 和 MIT PicoSHA2 固定来源/哈希不变；CRC 仍用平台 zlib。 本机 g++
16.1.0、CMake 4.0.1、Python 3.12.0；Android NDK 28.2.13676358/CMake 3.22.1/JDK 25。
OpenSSL/宿主运行库不是完整锁定工具链，实际后端字符串与构建参数保存在参考记录中。

新增 arm64/API24 独立 engine 测量目标，保留调度/存储/C ABI 并使用 fake transport； 与 baseline/digest
使用同一 `-Os`、静态 STL、section GC、导出列表、strip 和无 LTO 配置。 三个 strip
后共享库的字节数如下，NEEDED、源码和产物哈希见 [体积报告](m3/size-arm64.json)。

| 目标                      | 字节   |
|---------------------------|--------|
| 版本 baseline             | 208000 |
| 文件读取与 SHA-256        | 225256 |
| engine（含假传输）        | 489784 |
| engine 相对 baseline 差值 | 281784 |

普通双 ABI AAR 为 140409 字节，arm64 JNI 为 208768 字节，x86_64 为 208168 字节；NEEDED 包含系统 libz。
engine 目标不包含平台网络及 JNI 下载桥接，普通 AAR 的 JNI 也仍只引用版本接口，均不能代表完整 Android
下载接入成本。 手动 ASCII/UTC HTTP-date 解析避免引入 stream/locale 解析路径，保留三类日期与溢出测试。
Kotlin 依赖仍为 stdlib 2.2.10 和 annotations 13.0，普通构建没有实验网络依赖。

## 复现与剩余门槛

Windows 参考配置：

```powershell
cmake -S . -B build/m3-pinned -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc -DCMAKE_PREFIX_PATH=D:/msys64/ucrt64 -DCMAKE_BUILD_TYPE=Debug -DFETCHKIT_WITH_CURL=ON -DFETCHKIT_USE_PINNED_CURL=ON -DFETCHKIT_CURL_ARCHIVE=D:/work-path/android/fetchkit/build/third-party/curl-8.20.0.tar.xz -DFETCHKIT_BUILD_TESTS=ON -DFETCHKIT_BUILD_PROBES=ON -DFETCHKIT_ENABLE_FAULT_INJECTION=ON
cmake --build build/m3-pinned --parallel 2
ctest --test-dir build/m3-pinned --output-on-failure -LE large_file
python tools/ci/collect_m3_evidence.py
```

归档器验证全部 gate、当前体积输入哈希和 Android 成功记录后才归档；报告不自我包含在源码哈希集合中。
Linux 入口：

```sh
bash tools/ci/linux_m3.sh
FETCHKIT_SANITIZERS=address,undefined bash tools/ci/linux_m3.sh
FETCHKIT_SANITIZERS=thread bash tools/ci/linux_m3.sh
FETCHKIT_RUN_LARGE_FILE=1 bash tools/ci/linux_m3.sh
```

上述入口尚未在 Linux 执行。仍须补充 Linux 常规/竞态、分别运行 sanitizer、真实文件系统 ENOSPC 与长期压力记录；
系统重启/掉电、完整 Android 下载和设备生命周期不能由 Windows、故障注入或子进程 kill 代替。
