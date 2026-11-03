# 新请求重试契约（A0）

[文档中心](../README.md) · [功能补齐计划](android-sdk-completeness-plan.md)

2026-11-02，A0 固定 A1 的实施与验收边界，A1 按本契约实现。公开调用以
[主机 API](../reference/host-api.md) 和 [Android API](../reference/android-api.md) 为准。
验证结果按对应阶段报告判断；A0 的特征测试不代表恢复能力验收。

## 接口与准入

接口为 C ABI `fetchkit_task_retry(task, request, options)`、C++ RAII 包装和 Kotlin
`DownloadSubscription.retry(request): CompletableFuture<Unit>`。
options 为可空、带 struct_size 的独立请求选项，空值沿用默认策略；原请求结构、枚举编码和 enqueue/attach 保持兼容。
带 options 的入口为 `fetchkit_enqueue_with_options` / `fetchkit_attach_with_options`；
Kotlin 新选项有默认值，bridge 的有界编码与 SDK 同步升级。

| 共享状态 / 订阅状态 | 新请求重试结果 |
|--------------------|----------------|
| Paused | 校验并开始新 generation |
| Failed + AuthRequired / Network / Timeout / HttpStatus | 校验并开始新 generation |
| Running / Queued / RetryWaiting / Pausing / Verifying / Publishing / Completed | Busy |
| 调用订阅已取消或因溢出失效 | Cancelled |
| 调用订阅已释放、engine 关闭或身份已被新任务取代 | Closed |
| 身份、长度、摘要、requireDigest、业务引用冲突 | InvalidArgument |
| 请求、选项非法 | InvalidArgument；超出单文件容量返回 QuotaExceeded |

HttpStatus 的显式重试与自动重试分别处理；允许宿主修正 404/410 后重试，不将这些状态加入普通自动重试集合。
RetryWaiting 须先暂停并确认停止后再换请求。Cancelled/Removed 不通过旧句柄复活。
expiresAtMs 须与当前记录一致，pinOnCreate 在 retry 中不重新应用；策略变更使用 pin/setExpiry。

## 请求来源与原子准入

1. 新请求复制到调用者的订阅；只替换该订阅的凭证/provider，但该次执行仍影响共享任务及所有其他订阅。
   调用订阅成为本 generation 的首选来源，自动重试和鉴权刷新优先使用它，不再依赖插入顺序。
2. 首选来源取消/释放后，回退到最早的其他有效订阅。正在进入的 provider 先完成取消屏障，再交接；
   旧 provider 用户对象在 retry 成功后不再被访问。无有效来源遵循最后取消/释放的既有语义。
3. 锁顺序继续为 admission（准入串行化）→ engine → task → subscription。
   不在这些锁下执行 provider、用户回调或诊断输出；关闭先设关闭标志，再等待准入完成。
4. 先校验所有身份/约束、选项、状态、队列容量和 generation 上限，再准备 cancellation 和队列节点等可失败资源。
   存储保护准入成功后才一次性发布新请求、首选来源与新 generation；失败回滚新增保护，不修改旧请求/代次/检查点。
   provider 对象销毁移出核心锁，避免宿主捕获对象析构重入 SDK。
5. 不要求回写或持久化 provider/URL/headers。原 enqueue/attach 不变成后来的请求覆盖已有请求。
   Future / C ABI 成功只代表取得执行许可，最终结果由 query/observe/wait 确认。

## generation、预算与持久化

- 显式 retry 增加 generation，清空该轮 HTTP/platform/requestCount 错误与事件关联信息，恢复数据来自已提交前缀。
- 已耗普通重试次数保持；重试耗尽后显式调用仍能给予一次请求机会，但不会重获自动重试额度。
- 已记录的 Retry-After UTC 截止时间与进程内单调时钟限制保持，不因换请求、暂停或进程重开提前发送。
- 新 generation 获得最多一次鉴权刷新额度；普通自动重试沿用该 generation 的已耗刷新额度。
- committedOffset、强 ETag 和身份约束保持；新地址返回 200、无强校验器或不可靠 206 时沿用安全重下规则。
  跨 origin 清除隐式凭证，禁止 HTTPS 降级；新地址不代表可信内容已通过校验。
- v2 现有 retry_count、next_retry_at_ms、auth_refreshes 足够表达预算，不改记录格式；
  刷新白名单和首选订阅均是本进程请求选项，重开后由宿主重新提供。未知版本、损坏隔离与租约保护不变。

## provider 失败与刷新策略

- 默认刷新集合固定为 401/403；options 只允许增加 404、410，去重且最多两项。
  所有状态共享每 generation 一次额度，不开放任意响应体或签名 URL 日志。
- C provider 返回 Network/Timeout/AuthRequired/Cancelled 时保留分类；其余非 Ok 返回值与未知异常沿用 AuthRequired。
  Kotlin 增加仅接受上述四类状态的 RequestProviderException，JNI 只桥接受限错误码，不透传异常消息/堆栈。
- provider 返回 Cancelled 只结束当前请求，不等于宿主显式取消订阅，也不触发最后取消清理；已有取消/释放标志优先。
- 超时与取消依赖协作，不强行终止宿主代码。SDK worker 内同步 retry 返回 Busy，避免等待自身 provider 屏障。
- HTTP 200 错误页不能普遍识别；关键资源提供可信 SHA-256，摘要失败不发布。资源 expiresAtMs 不表示 URL 有效期。

## A0 基线与 A1 验收

A0 为 scheduler_storage 增加两个带 `known-limit-` 名称的特征场景，Android 矩阵增加对应两项：

- `known-limit-auth-rebind`：主机分别以 401/403 复现。先稳定提交 1024 字节，再耗尽鉴权刷新；
  新链接 enqueue 与 resume 仍不能执行，原前缀仍存在。Android 通过正式 Kotlin/JNI 重现失败后的重绑不执行。
- `known-limit-request-source`：已有订阅暂停后用新地址新增订阅，稳定确认仍调用旧 provider，而新 provider 没被选择。

这些断言只固定 A0 的已知缺口，必须在 A1 转为新 API 成功恢复的断言；不保留“必须失败”的永久产品要求。
正常刷新一次、共享单 writer、provider 释放屏障、预算持久化和事件 generation 继续由原测试覆盖。

A1 增加下列反向与竞争验收：

| 范围 | 必须验证的结果 |
|------|----------------|
| 准入 | 状态表、错误枚举、空请求、约束/身份冲突、队列满及释放/关闭后的句柄 |
| 回滚 | 失败不换 provider、不进新 generation、不丢前缀、不泄漏预留/保护 |
| 续传 | 失败后用新链接发送精确 Range/If-Range，可信摘要与租约全内容相符 |
| 来源 | 首选 provider 覆盖插入顺序；取消/释放后交接，不访问旧用户对象 |
| 预算 | 显式重试不清零普通预算；等待不提前；刷新状态共享一次额度；跨重开保持约束 |
| provider | 受限分类、未知异常、取消、协作超时；无 SDK 锁下调用 |
| 安全 | 跨域凭证、降级拒绝、404/410 默认与显式选项、200 错误页摘要拒绝 |
| 生命周期 | retry 与 pause/cancel/release/close 确定性交错；不增加 writer 或无界队列 |

具体命令与测试环境见 [测试说明](../../tests/README.md)。主机测试不能代替 Android 运行时验收；
构建/设备执行前检查实际可用内存，不足时只清理本次拥有的已完成进程，仍不足则跳过并报告。
