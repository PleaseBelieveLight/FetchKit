# FetchKit

FetchKit 是面向 Android 与 Linux 的 C++ 文件下载 SDK，提供断点续传、多任务调度、持久化重试、 SHA-256
校验和带配额的资源存储。Android 通过 Kotlin/JNI 接入，主机提供 C ABI、C++ 包装与 CLI。

当前为 `0.1.0` alpha 本地预览，接口尚未冻结，完整发布验收尚未通过。项目采用
[Apache-2.0](LICENSE)；接入、构建、测试与发布资料统一见 [文档中心](docs/README.md)。

## 最小演示

### Android 单文件下载

以下 Kotlin 函数在宿主后台线程调用，传入实际 HTTPS 文件地址。示例同步等待结果， 不可在主线程或 SDK
回调中调用；UI 的异步接入见 [Android API](docs/reference/android-api.md)。
依赖接入见 [接入与打包](docs/release/packaging.md#maven-aar-与外部-android-宿主)。

```kotlin
import android.content.Context
import com.luofelix.fetchkit.*
import java.io.File

fun download(context: Context, url: String): TaskSnapshot {
    val engine = FetchKit.open(
        EngineConfig(
            storageRoot = File(context.noBackupFilesDir, "fetchkit-demo"),
            maxDownloadBytes = 64L * 1024 * 1024,
            storageCapacityBytes = 128L * 1024 * 1024,
            stagingRetentionMs = 7L * 86400000,
        )
    ).get()
    return try {
        val subscription = engine.enqueue(
            DownloadRequest(
                identity = ResourceIdentity("demo", "file", "1"),
                request = RequestInfo(url),
            )
        ).get()
        val result = subscription.awaitStopped().get()
        check(result.state == TaskState.Completed) { "下载未完成：${result.error}" }
        result
    } finally {
        engine.closeAsync().get() // 关闭引擎并释放其订阅，保留已提交的资源。
    }
}
```

资源身份由业务域、key、版本和内容变体组成，URL 不参与身份；下载不同内容时应更换身份。
`Completed` 表示已校验发布，可信 SHA-256 匹配另由 `DigestVerified` 表示；读取文件须先获取租约。

### Android 多文件下载与列表进度

一次入队多个请求，由同一个引擎调度，最多同时下载 3 个文件。以下函数可从主线程调用；
`onUpdate` 可选，传入时在主线程通知对应资源的进度、状态和错误，不传则不观察事件。

```kotlin
import android.content.Context
import android.os.Handler
import android.os.Looper
import com.luofelix.fetchkit.*
import java.io.File
import java.util.concurrent.CompletableFuture
import java.util.concurrent.Executor

fun downloadFiles(
    context: Context,
    requests: List<DownloadRequest>,
    onUpdate: ((ResourceIdentity, TaskEvent) -> Unit)? = null,
): CompletableFuture<DownloadEngine> {
    val handler = Handler(Looper.getMainLooper())
    val mainExecutor = Executor { handler.post(it) }
    val config = EngineConfig(
        storageRoot = File(context.noBackupFilesDir, "fetchkit-batch-demo"),
        maxDownloadBytes = 64L * 1024 * 1024,
        storageCapacityBytes = 256L * 1024 * 1024,
        stagingRetentionMs = 7L * 86400000,
        maxActiveDownloads = 3,
    )
    return FetchKit.open(config).thenCompose { engine ->
        val submissions = requests.map { request ->
            engine.enqueue(request).thenAccept { subscription ->
                if (onUpdate != null) {
                    subscription.observe(mainExecutor) { event ->
                        onUpdate(request.identity, event)
                    }
                }
            }
        }
        CompletableFuture.allOf(*submissions.toTypedArray()).handle { _, error ->
            if (error == null) CompletableFuture.completedFuture(engine)
            else engine.closeAsync().thenApply<DownloadEngine> { throw error }
        }.thenCompose { it } // 部分入队失败也先关闭引擎，释放已创建的订阅。
    }
}
```

调用示例（将地址替换为实际 HTTPS 文件地址）：

```kotlin
val batch = downloadFiles(context, listOf(
    DownloadRequest(ResourceIdentity("demo", "photo", "1"), RequestInfo("https://example.com/photo.jpg")),
    DownloadRequest(ResourceIdentity("demo", "video", "1"), RequestInfo("https://example.com/video.mp4")),
)) { identity, event ->
    // 主线程：按完整 identity 定位列表项，更新 receivedBytes、totalBytes、state 和 error。
    // RecyclerView 按需 notifyItemChanged(position, "progress")；Compose 更新对应条目的状态。
}
```

进度事件约每 200 ms 合并一次，`totalBytes == null` 或为 0 时不计算百分比；回调保持轻量且不抛异常。
`batch` 完成表示请求已入队，下载结果通过事件中的 `state/error` 判断。
页面或 ViewModel 生命周期结束时执行 `batch.thenCompose { it.closeAsync() }`，释放全部订阅并尽力保留未完成检查点；
宿主须处理入队及关闭 Future 的异常。单独停止观察、暂停或取消的用法见
[订阅与生命周期](docs/reference/android-api.md#订阅事件与恢复)。

## 架构说明

```text
Kotlin API / C ABI / C++ 包装 / CLI
                  │
        C++ 核心：任务调度、续传、重试
            ┌─────┴─────┐
       网络适配层       资源存储
    Android: OkHttp    检查点、SHA-256、原子发布
    Linux: libcurl     配额、LRU、pin、过期、只读租约
```

C++ 核心决定下载与存储策略；Android 层负责 JNI、网络适配，宿主负责生命周期、系统后台执行许可与通知。
详细设计见 [设计基线](docs/design/v1-design.md)。

## SDK 核心接口

| 接口                                                         | 用途                                                                       |
|--------------------------------------------------------------|----------------------------------------------------------------------------|
| `FetchKit.open(config)`                                      | 异步打开引擎并扫描恢复记录，同目录只允许一个所有者                         |
| `engine.enqueue(request)` / `attach(request)`                | 创建独立订阅；前者给予执行许可，后者只绑定，恢复记录保持暂停               |
| `subscription.observe(...)` / `query()`                      | 观察事件或查询状态、进度、检查点与错误                                     |
| `subscription.pause()` / `resume()`                          | 控制共享任务；仅 `Paused` 确认检查点成功                                   |
| `subscription.cancel()` / `releaseAsync()`                   | 取消或静默释放当前订阅；最后显式取消清理未完成任务，最后释放尽力保留检查点 |
| `engine.listTasks()`                                         | 分页列出持久化任务，由宿主重新提供请求并决定是否恢复                       |
| `engine.acquire(id)`                                         | 获取有效 Completed 对象的只读租约，读取后调用 `lease.releaseAsync()`       |
| `engine.pin(...)` / `setExpiry(...)` / `remove(...)`         | 管理资源保留、过期与删除，遵守活动任务和租约保护                           |
| `engine.storageStats()` / `trimTo(...)` / `collectStaging()` | 查询配额账本、按 LRU 回收与清理过期暂存                                    |
| `engine.closeAsync()`                                        | 停止 I/O 并释放引擎；已持有的租约须另行释放                                |

完整的参数、线程、错误与所有权契约见 [Android API](docs/reference/android-api.md) 和
[主机 C/C++ API](docs/reference/host-api.md)。
