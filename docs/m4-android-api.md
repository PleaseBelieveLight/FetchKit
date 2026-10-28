# M4 Android 接入

状态：alpha，2026-10-27。Kotlin/JNI 复用 M3 核心调度、协议、存储、重试与租约；保留 `FetchKit.version()`。
运行矩阵、产物和未验证的环境见 [M4 报告](reports/m4-baseline.md)。

## 打开与请求

```kotlin
val config = EngineConfig(
    storageRoot = File(context.noBackupFilesDir, "fetchkit"),
    maxDownloadBytes = 2L * 1024 * 1024 * 1024,
    storageCapacityBytes = 4L * 1024 * 1024 * 1024,
    stagingRetentionMs = 7L * 86400000,
)
FetchKit.open(config).thenCompose { engine ->
    engine.enqueue(DownloadRequest(
        identity = ResourceIdentity("models", "encoder", "2026-10"),
        request = RequestInfo("https://example.com/model.bin"),
        pinOnCreate = true,
    ))
}.thenAcceptAsync({ subscription ->
    // Save the subscription and engine in the owner's lifecycle scope.
}, mainExecutor)
```

`open`、入队/绑定、查询/分页、控制、存储策略、读取和关闭均返回 Future，磁盘与同步不在调用线程执行。
不要在主线程 `.get()` / `.join()`。Future 的非 Async continuation 可能在 SDK 完成线程上执行；
UI 使用 `thenAcceptAsync(..., mainExecutor)`，组合操作使用 `thenCompose`，不要在完成回调中阻塞等待另一项 API。
取消 Future 不等同于取消订阅；有资源返回值的提交 Future 拒绝取消，避免成功创建后丢失待释放对象。

每进程最多打开 16 个 Kotlin engine；公共 API 工作池 2 个线程、256 个等待槽，关闭使用独立控制通道。
每 engine 1 个事件泵与 1 个取消监视线程，核心线程限制和队列见 [M3 API](m3-host-api.md)。
同一订阅只有一个待完成的 `awaitStopped` 轮询，不因重复等待累积无界任务。
SDK 不引入 Compose、WorkManager 或通知依赖。
宿主可通过 `EngineConfig.network` 显式绑定 Android Network；socket 与 DNS 使用同一网络，不自动切换到其他网络。
后台示例使用 JobParameters 的获准网络；API 24～27 从满足约束的网络中选取，不能仅因另一个 Wi-Fi 存在就走计费默认网络。

## 订阅、事件与恢复

`DownloadSubscription` 是独立订阅；相同身份共享一个 writer。`pause` / `resume` 影响共享任务，
`cancel` 仅撤销当前订阅，`releaseAsync` 静默释放；最后订阅释放保留暂停记录，最后显式取消清理未完成任务。

```kotlin
val observation = subscription.observe(mainExecutor) { event ->
    // Immutable core event: taskId, generation, state, bytes, integrity and error.
}
subscription.pause().thenAcceptAsync({ paused ->
    // Paused confirms I/O stopped and the checkpoint committed.
}, mainExecutor)
subscription.resume()
subscription.cancel()
observation.close()
subscription.releaseAsync()
engine.closeAsync()
```

每订阅串行分发，最多一个事件批次等待宿主 executor；未分发事件留在核心 64 项队列并采用 M3 的合并/溢出规则。
关闭或释放后排队的 Java 回调检查失效标记并丢弃，已进入的宿主回调由宿主完成。
宿主 executor 拒绝或 listener 抛异常时停止该 observer 并取消其订阅，不向其他订阅传播异常。
事件包含 generation，调用方按 taskId/generation 关联业务状态；事件不携带 URL、headers 或文件路径。

`closeAsync` 首先阻止新调用，等待已经进入的入队/控制事务，然后停止 I/O、提交记录并释放订阅/native provider。
Future 完成发生在 SDK 锁之外。关闭不依赖宿主执行积压事件，也不重新创建旧进程的回调。
每轮运行的 provider 在 C++ worker 上协作执行：

```kotlin
val provider = RequestProvider { context ->
    if (context.isCancelled()) throw IllegalStateException("Cancelled")
    // Fetch/copy current request credentials. Cooperate with cancellation.
    RequestInfo(context.proposedUrl, mapOf("Authorization" to currentToken))
}
```

不要在 provider 内阻塞等待 engine API Future，组合控制使用异步调用。provider 异常映射为 AuthRequired；
解除订阅等待已经进入的 provider 返回，之后不再访问其用户对象。
`isCancelled` 使用调用范围内的整数注册表，范围结束后返回 true，不暴露 native 指针。
记录不保存 provider、URL、headers、Kotlin 对象或旧 token。重开后 `listTasks` 查询身份/快照，
宿主以原约束重新提供 `DownloadRequest`，选择 `attach` 保持暂停或 `enqueue` 明确给予执行许可。

## 网络与 JNI

正式后端使用 M0 已选择的独立 OkHttp 5.5.1；本阶段该依赖进入普通 SDK 的运行依赖。
AAR 不嵌入其传递依赖，直接文件接入还须声明对应依赖；完整增量含 OkHttp、Okio、Kotlin 与实际解析的 AndroidX 组件。
SDK Manifest 声明 INTERNET；HTTP 仍须 engine 显式允许，并由宿主 Android 网络安全策略另行批准。

客户端不读取全局 Cookie/Authenticator，不跟随重定向，不做连接重试，不使用缓存或隐式代理，始终声明 identity。
OkHttp 5.5.1 仍可能自动处理 `503 + Retry-After: 0`；适配层在网络 interceptor 捕获真实状态，
在内部策略阶段使用不可跟进状态，向 C++ 回传原始状态及原始头/body。源行为依据
[固定版本源码](https://github.com/square/okhttp/blob/parent-5.5.1/okhttp/src/commonJvmAndroid/kotlin/okhttp3/internal/http/RetryAndFollowUpInterceptor.kt)。
所有重试、鉴权、重定向和续传仍由 C++ 决策，测试同时断言服务器请求次数。
OkHttp 只能提供解码后的 header String；为避免不同原始 ETag 被替换字符折叠为相同值，当前 Android alpha
保守拒绝非 ASCII ETag，返回 ProtocolMismatch。该限制不影响 UTF-8 资源身份，也不把不可靠的 validator 用于追加。

单轮请求复用 64 KiB ByteArray，跨 JNI 同步复制到 64 KiB native 块；不保存借用指针或缓存整文件。
响应头必须先获核心接受，拒绝后不交付 body。监视器每 25 ms 检查取消、连接和完整响应头截止时间，
读空闲使用 socket timeout，不把完整下载套进短的总超时。
系统 DNS 可能忽略 Java 中断，因此由全局 4 个 worker / 64 个队列槽隔离；调用方有截止时间并可取消等待。
已进入的系统解析可能继续到平台返回，其 worker 只持 hostname/值，不保留 operation、sink、JNI/provider 用户数据。
这个边界不表示能够中断操作系统 DNS 服务本身。

JNI 使用 RegisterNatives、整数句柄与有界参数；只导出 JNI_OnLoad。新线程按需附着并配对释放，
回调局部引用有作用域，provider 使用强全局引用直到解除屏障，Java/C++ 异常不会跨 native ABI。
内部控制/快照使用有界值数组；公共 Kotlin 类型不暴露该编码或 native 路径。
字符串桥使用标准 UTF-8/UTF-16 转换，支持中文与补充平面字符，拒绝 NUL/孤立 surrogate。
consumer rules 保留按名称注册/调用的 bridge、provider 与网络方法；Release 宿主须实测 R8 后接入。

## 读取、缓存和存储位置

`engine.acquire(id)` 返回 `ResourceLease`；`read(offset, capacity)` 单次 1～65536 字节、异步返回 ByteArray。
循环读取或导出后调用 `releaseAsync`。lease 在 engine 关闭后仍可读并保持根目录所有权；
最后一个 lease 释放前，同目录新 engine 返回 Busy。
pin、setExpiry、remove、trimTo、collectStaging 的保护和错误语义与 M3 相同。
Completed/TransportComplete 与 DigestVerified 分别呈现；不要把前者说成可信摘要验证。

普通缓存可用 cacheDir，但系统可自行回收；pin 仅约束 SDK 淘汰。长期离线内容选择 filesDir/noBackupFilesDir，
工作目录及对象必须在同一文件系统。SDK 不擅自修改宿主备份配置。
示例使用 filesDir/fetchkit，并在旧备份规则、云备份和设备迁移中显式排除整个目录。
部分恢复的记录或对象必须经过核心扫描修复，不能仅凭路径恢复认定资源有效。

## 示例的后台执行许可

示例分为下载、存储和能力指南三个页面，操作与验证方式见 [Android 示例指南](android-sample.md)。
前台页面显示多文件进度、采样速度、核心快照、检查点、重试信息和有界状态事件，提供批量/逐任务暂停恢复、
独立订阅取消/释放、可选长度/SHA-256、pin/过期、LRU 回收、暂存清理和可手动释放的读取租约。
每个资源使用自己的请求与订阅，控制现有任务不会再从公共输入框构造其他资源的请求。
页面离开且没有后台任务时释放租约并关闭 engine；重新打开只扫描记录，用户明确恢复后才运行。

预设请求的资源 ID、版本、长度、可信摘要和创建时 pin 配置由宿主异步保存到 noBackupFilesDir，使用 AtomicFile；
URL 从固定预设目录重建。SDK 记录格式不变，仍不保存 URL、凭证或回调。
自定义请求仅存在内存中，重启后需要重新提供原约束并绑定对应身份。
后台示例接受固定预设（包括带 bytes 参数的 Cloudflare 资源），Job extras 只保存预设 ID 与非敏感约束；
自定义后台地址仍限没有 userinfo/query/fragment 的公开地址。签名 URL 和凭证应由宿主 provider 重新获取。
示例一次只排队一个系统任务（相同 Job ID 的新排队会替换旧排队），前台批量下载使用 SDK 调度。

用户可见操作在 API 34+ 使用 JobScheduler UIDT，并声明 RUN_USER_INITIATED_JOBS、网络约束与通知；
低版本使用有限、可中断的普通 JobScheduler 任务，不保证立即启动或长时间运行。
该选择遵循 [Android 后台数据传输说明](https://developer.android.com/develop/background-work/background-tasks/data-transfer-options)
和 [UIDT 文档](https://developer.android.com/develop/background-work/background-tasks/uidt)。
本示例没有引入 WorkManager；需要旧版本长期前台传输的宿主应选择适合其产品的独立调度适配。

onStopJob 发起异步关闭，停止 I/O 并尽力提交；再次取得系统执行机会后以当前请求信息重新绑定。
系统调度仅重给执行机会，不在 Kotlin 中再实现一套 HTTP 重试。普通进程终止、系统停止、用户强制停止、重启和掉电分别记录，
强制停止后不承诺自动恢复；下一次用户/系统合法执行机会才可重新执行。
