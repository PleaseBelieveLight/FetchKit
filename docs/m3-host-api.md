# M3 调度、订阅与资源存储

状态：alpha，2026-10-27。C ABI 增加字段后须重新编译客户端，尚未冻结发布版 ABI。
主机实现已加入多任务、重试和缓存管理；Android Kotlin/JNI 下载接入仍待 M4。
验证范围与缺测项见 [M3 报告](reports/m3-baseline.md)。

## 配置与运行

宿主须明确设置 `storage_root`、`max_download_bytes`、`storage_capacity_bytes` 和
`staging_retention_ms`。 容量按 SDK 管理的文件逻辑字节及预留计费，包括暂存、最终对象、孤立/隔离文件和受控元数据预算。
该账本不代替文件系统剩余空间或物理块计费；实际写入/同步仍可能返回 DiskFull、PermissionDenied 或
IoError。

| 项目                        | 默认值 / 上限                                                       |
|-----------------------------|---------------------------------------------------------------------|
| 活动下载（含本轮校验/发布） | 3 / 16                                                              |
| 待运行任务                  | 1024 / 1024；活动任务不占普通等待槽                                 |
| 每 engine 未释放订阅        | 1024                                                                |
| 每任务未释放订阅            | 64；已取消但未 release 仍计数                                       |
| 每存储上下文读取租约        | 1024                                                                |
| 每订阅事件                  | 64 个紧凑事件，进度约每 200 ms 合并                                 |
| listTasks 页容量            | 1～100                                                              |
| 每记录预算                  | 131112 字节，覆盖两份最大记录；不与实际元数据重复计费               |
| 未知长度申请块              | 64 KiB                                                              |
| 普通重试                    | 最多额外 3 次；1 秒起步、指数退避及正向随机抖动，本地等待上限 30 秒 |

任务队列是有界 FIFO；到期的 RetryWaiting 才可运行，等待期间不占下载工作线程。 每 engine 固定创建
`max_active_downloads` 个工作线程和 1 个控制线程，后者处理排队任务的暂停/取消，
避免所有网络线程停顿时控制请求饿死。网络后端可能另有解析线程，实测总线程数单列。 传输背压是同步
Sink，SDK 不缓存整文件；Verifier 以 64 KiB 块重新读取内容。

CLI 示例：

```powershell
.\build\m3-pinned\fetchkit_cli.exe download "https://example.com/model.bin" --root build/downloads --key model-v1 --capacity 17179869184 --active 3 --retries 3 --sha256 <可信的64位摘要> --require-digest
.\build\m3-pinned\fetchkit_cli.exe shell --root build/downloads --capacity 17179869184
```

CLI 自身选定 16 GiB 容量和 7 天暂存保留期；库的 init 不替宿主填这两个必填参数。
`--pin` 在提交任务时固定保留；shell 中该选项对本进程的新任务生效。 调试可用 `--retry-base-ms`、
`--retry-max-ms`、`--retention-ms`；它们不会缩短合法 Retry-After。 本地 HTTP 仍须显式 `--allow-http`。普通
API/重定向/鉴权规则沿用 M1/M2。

## 可选诊断日志

日志默认关闭，由宿主显式设置当前库实例内的进程级级别，不写入存储记录：

```c
fetchkit_set_log_level(FETCHKIT_LOG_WARN);
/* 排障结束后关闭；也可选择 ERROR 或 DEBUG，级别包含更严重的消息。 */
fetchkit_set_log_level(FETCHKIT_LOG_OFF);
```

C++ 可使用 `fetchkit::setLogLevel(fetchkit::LogLevel::Warn)`。设置线程安全，非法级别返回
InvalidArgument 并保留原设置；切换影响后续日志，已进入输出的日志可完成。静态链接到不同模块的独立库实例各自配置。
主机写 stderr，CLI 的 stdout JSON 不受影响。日志只覆盖打开/关闭、扫描修复汇总、重要续传回退、
每轮重试和最终失败/发布完成；不打印数据块、读写、轮询或进度。Error 表示操作失败，Warn 表示可恢复异常，
Debug 增加正常操作边界；暂停和取消不会被记录为错误。

任务日志包含核心生成的 taskId/persistentId、generation、状态/错误码、HTTP 状态、平台码和重试信息。
不输出资源身份原文、路径、URL、headers、响应体或原始异常文本。日志不代替返回值、快照、事件，也不增加
宿主回调、线程或日志队列；核心调度锁外完成诊断格式化，数据高频路径不输出日志。

## 任务与独立订阅

`fetchkit_task` 现在表示独立订阅 token。`enqueue` 相同身份只创建一个写入任务，返回不同 token； 身份仍为
namespace/key/version/representation 四元组，URL 和凭证不参与身份。 长度、可信摘要、require_digest
或业务地址引用冲突返回 InvalidArgument，不采用最后一次请求覆盖。

- `pause` / `resume` 控制共享任务整体，所有有效订阅观察同一进度和 generation。
- `cancel(token)` 仅撤销该订阅并产生其 Cancelled；其他订阅继续。最后一个有效订阅显式取消才取消并清理未完成任务。
- `release(token)` 静默解除订阅；最后一个释放时尽力暂停并保留检查点。释放后的 token 返回 Closed。
- 已完成对象不因订阅 cancel/release 而删除。进入 Publishing 后，取消仍可撤销订阅，但已经串行确认的发布继续完成。
- Paused 才确认 I/O 停止和检查点成功。排队任务控制使用保留的控制通道；普通队列满时新入队/恢复返回
  Busy。
- 任务关闭/暂停/取消优先于普通重试。不同订阅不能通过取消互相撤销。

provider 在 worker 上协作执行，不持 SDK 锁。每次选择仍有效订阅的 provider；释放某个订阅会等待其已进入的
provider 返回， 之后不再使用其用户指针。切换 provider 只复制请求值，不恢复旧进程的回调/凭证。 provider
context 的 task_id 是本次被选中订阅的 token，可在回调内 query/pause/cancel；worker 上同步
wait/release/close 返回 Busy。 超时只能在协作回调返回后检查，不能强制终止任意宿主代码。

进程重开后任务不自动执行。宿主通过 listTasks/attach 重新绑定，或 enqueue 明确允许运行。 显式新运行增加
generation；普通重试保持 generation。任务被 remove/过期暂存清理后重新创建时，旧订阅只保留历史查询，
不能控制替代任务；旧订阅的取消也不会删除新任务的数据。

## 宿主 executor 与事件

M3 采用宿主主动分发：宿主在自己选定的 executor 上调用
`fetchkit_task_dispatch(token, callback, user, max_events, &count)`。 SDK 不保存 callback/user
指针到调用结束之后，不为每订阅启动回调线程，不调用外部 post 函数。 这样慢 executor 或内联执行的宿主不会阻塞下载控制和
engine close；调度策略与原契约目标一致，接入形式明确为拉取分发。

```cpp
// 在宿主选定的 executor 上调用；一次最多分发 64 个。
task.dispatch([](void* context, const fetchkit_event* event) {
    // event 仅在此回调期间借用；需要留存则复制。
    // 使用 task_id / generation 关联 UI 或业务状态。
}, context);
```

每订阅串行分发，并发 dispatch 返回 Busy；回调内不持 SDK 锁，可重入 query/pause/cancel/close。
同一回调中同步释放自己返回 Busy，避免自等待。跨线程 release 等待已经进入的分发和 provider 静止。 engine
close 等待 SDK 自有 I/O，清除未来事件，不依赖宿主执行积压回调；已经进入的宿主调用由宿主负责完成。
关闭还会等待已进入的任务入队事务并建立存储停写屏障；已声明的读取租约上下文独立保留。

事件为 State、Progress、SubscriptionError，仅含数值状态、字节数、错误、代次和重试信息，不包含 URL/凭证/路径。
连续进度合并，状态/终态按订阅顺序保留，每 generation 最多一个终态。 状态容量耗尽时只终止该订阅，保留一次
SubscriptionOverflow 通知；其他订阅继续，最后一个溢出时共享任务暂停。 回调抛出的 C++ 异常被隔离为该订阅的
InternalError，不跨 C ABI。 宿主应持续分发或明确释放订阅，不能用无限队列容忍永远不工作的消费者。

## 重试与恢复

可重试：Network、Timeout、HTTP 408/429/500/502/503/504。证书、协议、长度/摘要、磁盘、权限和配额错误不参加普通重试。
鉴权刷新每 generation 最多一次，与普通重试分开计数并落盘；一次协议安全重下仍遵守 M1 的限制。

Retry-After 接受 delay-seconds 和 HTTP-date（含两类旧格式），依据
[RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html#name-retry-after) 处理，使用 UTC，不改变进程时区或
locale。 本地基础延迟按指数增长并加入最多 25% 的正向抖动，封顶 30 秒；服务端要求更晚时取更晚时间，不按本地上限提前请求。
合法但极远的等待保守保留到 INT64_MAX 毫秒，不溢出为立即重试。

retry_count、next_retry_at_ms 和鉴权预算存入 v2 记录。重启/暂停不能清零已消耗预算或 Retry-After 截止时间。
进程内同时使用单调时钟保护等待，墙钟前跳不提前绕过等待；重开后按持久化 UTC 截止时间重建剩余等待。
宿主重新给予执行许可后，未来截止时间仍须等待；显式恢复已耗尽预算的失败任务只允许当前这次手动尝试，不重新赠送自动重试次数。
显式 cancel/remove 后重建属于新任务；旧任务不会自动复活。 单 generation 的 max_requests 同时约束重定向、局部
206 和普通重试的请求总数。

## 配额、读取与淘汰

新记录先申请元数据预算。排队时不为所有文件同时预留整个大小；开始 attempt 时、响应确认总长时先预留已知总长，
未知长度在写入前以 64 KiB 向上取整申请。所有申请与淘汰在同一存储账本中串行，不能并发超卖。
部分写入/网络失败/暂停后，未使用的数据预留释放，实际暂存和元数据继续计费；重启重新按文件和记录构建账本，预留不重复恢复。
`stats` 的 used 是已观察到的完成写入和元数据字节，reserved 包括额外数据/元数据预留，charged 为决策计费总数。
同步或清理失败时可保留保守预算到重开扫描，不能提前释放仍占用的空间。

需要长期保留的下载使用 `request.pin_on_create=1`，把 pin 与入队记录原子提交，避免完成后才 pin 的窗口。
`resource_pin` 可动态 pin/unpin；pin 不绕过完整性、过期或资源身份。

- acquire 只接受有效 Completed，返回有计数的只读 lease。文件确认和计数与删除互斥；存在 lease 时不能
  remove/淘汰/替换。
- engine close 后 lease 仍可读取并保留目录锁，最后一个 lease 释放后锁才释放。
- 每次成功 acquire 持久化访问序号，发布也设置初始顺序；LRU 淘汰最久未使用、无 pin/lease/活动任务保护的
  Completed。
- 过期对象拒绝新 acquire，已存在 lease 继续可读。过期刷新必须重新校验发布；有 lease 或 pin 时拒绝替换。
- 对已过期对象再次 enqueue 是显式刷新，采用本次 expires_at_ms（0 表示不再设置到期时间）；未过期对象的策略更新使用
  set_expiry。
- remove 不能删除活动、pin 或 leased 对象；trimTo 同样尊重这些保护。无足够可淘汰空间返回
  QuotaExceeded，可能已经回收部分合格对象。
- remove 先提交 Removed tombstone，再删除数据和记录；退出后扫描完成清理，不会复活已移除资源。
- collect_staging 只回收超过保留期、无活动/pin/lease 保护的有记录暂存。重开不刷新它的年龄。
- 未知/隔离文件、无可信记录的 M1 数字 `.part` 仍保留并计费，不靠文件名猜测所有权后删除。

Completed 事件表示该次传输已通过发布；它不是永久保留承诺。缓存可被随后调度的任务淘汰，读取前取得 lease，
要求长期保留时使用 pin_on_create。query 的 available 与历史任务 Completed 状态分开表示。

shell 新增：

```text
events <订阅 token>
stats
pin "<persistent_id>"
unpin "<persistent_id>"
expiry "<persistent_id>" <UTC毫秒，0为无到期时间>
remove "<persistent_id>"
trim <目标计费字节数>
collect
```

记录兼容见 [v2 格式](storage-record-v2.md)。Linux/sanitizer、真实小容量文件系统和设备验证的缺测项仍阻断对应保证，
不将 Windows 测试、故障注入或进程终止外推为系统重启/掉电结果。
