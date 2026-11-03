package com.luofelix.fetchkit.coroutines

import com.luofelix.fetchkit.DownloadEngine
import com.luofelix.fetchkit.DownloadRequest
import com.luofelix.fetchkit.DownloadSubscription
import com.luofelix.fetchkit.EngineConfig
import com.luofelix.fetchkit.FetchKit
import com.luofelix.fetchkit.FetchKitException
import com.luofelix.fetchkit.ResourceLease
import com.luofelix.fetchkit.Status
import com.luofelix.fetchkit.TaskSnapshot
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionException
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.time.Duration.Companion.milliseconds

/**
 * 等待无句柄的 Future，例如 query、pause 或导出结果。取消等待不会取消底层操作。
 * open、enqueue、attach 和 acquire 应使用下方的作用域 API 管理句柄。
 */
suspend fun <T> CompletableFuture<T>.awaitValue(): T = suspendCancellableCoroutine { continuation ->
    whenComplete { value, error ->
        if (error == null) continuation.resume(value)
        else continuation.resumeWithException((error as? CompletionException)?.cause ?: error)
    }
}

internal suspend fun <T : Any, R> withOwned(
    create: () -> CompletableFuture<T>,
    release: (T) -> CompletableFuture<Unit>,
    block: suspend (T) -> R,
): R {
    currentCoroutineContext().ensureActive()
    var owned: T? = null
    var failure: Throwable? = null
    try {
        // 在不可取消段内接收所有权，避免 Future 成功与协程取消竞争时丢失 native 句柄。
        withContext(NonCancellable) { owned = create().awaitValue() }
        currentCoroutineContext().ensureActive()
        return block(checkNotNull(owned))
    } catch (error: Throwable) {
        failure = error
        throw error
    } finally {
        owned?.let { value ->
            try {
                withContext(NonCancellable) {
                    while (true) {
                        try {
                            release(value).awaitValue()
                            break
                        } catch (error: FetchKitException) {
                            // API 队列可能暂满。保留所有权直到释放被接受，不把清理工作分离到全局 scope。
                            if (error.status != Status.Busy) throw error
                            delay(10.milliseconds)
                        }
                    }
                }
            } catch (error: Throwable) {
                if (failure == null) throw error
                if (failure !== error) failure.addSuppressed(error)
            }
        }
    }
}

/** 打开引擎并在 block 结束后关闭。引擎只在 block 内借用。 */
suspend fun <R> withDownloadEngine(config: EngineConfig, block: suspend (DownloadEngine) -> R): R =
    withOwned({ FetchKit.open(config) }, DownloadEngine::closeAsync, block)

/**
 * 在 block 内借用独立订阅。start=false 只绑定，start=true 授予执行许可。
 * 退出时静默释放订阅，协程取消不会显式取消共享任务。
 */
suspend fun <R> DownloadEngine.withSubscription(
    request: DownloadRequest, start: Boolean = true, block: suspend (DownloadSubscription) -> R,
): R = withOwned({ if (start) enqueue(request) else attach(request) }, DownloadSubscription::releaseAsync, block)

/** 在 block 内借用可用资源的读取租约。退出时释放，包括创建与协程取消竞争的情况。 */
suspend fun <R> DownloadEngine.withResourceLease(
    persistentId: String, block: suspend (ResourceLease) -> R,
): R = withOwned({ acquire(persistentId) }, ResourceLease::releaseAsync, block)

/** 一个订阅 owner 持有一个桥接。null 表示尚无快照，关闭后保留最后一个值。 */
class SnapshotObservation internal constructor(val snapshots: StateFlow<TaskSnapshot?>, private val job: Job) :
    AutoCloseable {
    /** 停止观察，订阅仍由 owner 释放。 */
    override fun close() {
        job.cancel()
    }
}

/**
 * 每个订阅只调用一次，多个页面或 collector 重用 [SnapshotObservation.snapshots]。
 * 无 collector 时仍排空事件。观察器关闭或 scope 结束后停止查询。
 * 查询失败会通过 scope 报告；API 队列暂满时重试。完整状态事件应直接使用 observe。
 */
fun DownloadSubscription.observeSnapshots(scope: CoroutineScope): SnapshotObservation {
    val state = MutableStateFlow<TaskSnapshot?>(null)
    val changed = Channel<Unit>(Channel.CONFLATED)
    val observer = observe({ it.run() }) { changed.trySend(Unit) }
    changed.trySend(Unit)
    val job = scope.launch {
        for (ignored in changed) {
            // 初始查询与事件触发的查询始终串行。事件只标记需要刷新，不直接回写旧 generation。
            try {
                state.value = query().awaitValue()
            } catch (error: FetchKitException) {
                if (error.status == Status.Closed) break
                if (error.status != Status.Busy) throw error
                delay(10.milliseconds)
                changed.trySend(Unit)
            }
        }
    }
    job.invokeOnCompletion {
        observer.close()
        changed.close()
    }
    return SnapshotObservation(state.asStateFlow(), job)
}
