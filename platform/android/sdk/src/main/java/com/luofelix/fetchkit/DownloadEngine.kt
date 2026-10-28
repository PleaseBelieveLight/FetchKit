package com.luofelix.fetchkit

import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.CompletableFuture
import java.util.concurrent.Executor
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

internal object EngineIo {
    val openSlots = java.util.concurrent.Semaphore(16)
    val executor = ThreadPoolExecutor(2, 2, 30, TimeUnit.SECONDS, ArrayBlockingQueue(256),
        { Thread(it, "fetchkit-api").apply { isDaemon = true } }, ThreadPoolExecutor.AbortPolicy())
    val controls = ThreadPoolExecutor(1, 1, 30, TimeUnit.SECONDS, ArrayBlockingQueue(16),
        { Thread(it, "fetchkit-close").apply { isDaemon = true } }, ThreadPoolExecutor.AbortPolicy())
    fun <T> submit(block: () -> T): CompletableFuture<T> {
        val result = object : CompletableFuture<T>() { override fun cancel(mayInterruptIfRunning: Boolean) = false }
        try { executor.execute { try { result.complete(block()) } catch (e: Throwable) { result.completeExceptionally(e) } } }
        catch (_: java.util.concurrent.RejectedExecutionException) { result.completeExceptionally(FetchKitException(Status.Busy)) }
        return result
    }
}

/**
 * 所有磁盘与控制操作异步执行；Future 完成回调可能直接运行在 SDK 线程，禁止在回调中阻塞等待另一项 API。
 * 取消 Future 不代表取消下载；返回订阅/lease 的提交 Future 拒绝取消，调用方必须释放成功返回的资源。
 */
class DownloadEngine internal constructor(private val handle: Long, private val transport: AndroidTransport) {
    private val closing = AtomicBoolean()
    private val tasks = java.util.concurrent.ConcurrentHashMap<Long, DownloadSubscription>()
    internal val pump = ScheduledThreadPoolExecutor(1) { Thread(it, "fetchkit-events").apply { isDaemon = true } }
        .apply { removeOnCancelPolicy = true }
    private val closeResult = CompletableFuture<Unit>()
    private val calls = java.util.concurrent.locks.ReentrantReadWriteLock(true)

    internal fun <T> nativeRead(block: () -> T): T {
        if (closing.get()) throw FetchKitException(Status.Closed)
        val lock = calls.readLock()
        if (!lock.tryLock()) throw FetchKitException(Status.Closed)
        return try { if (closing.get()) throw FetchKitException(Status.Closed); block() }
        finally { lock.unlock() }
    }
    internal fun <T> operation(block: () -> T): CompletableFuture<T> = EngineIo.submit { nativeRead(block) }
    /** 创建独立订阅并给予执行许可；同身份共享 writer，原约束冲突时失败，不覆盖已有任务。 */
    fun enqueue(request: DownloadRequest): CompletableFuture<DownloadSubscription> = bind(request, true)
    /** 重新绑定请求与约束，恢复记录保持暂停；已运行的共享任务继续运行。 */
    fun attach(request: DownloadRequest): CompletableFuture<DownloadSubscription> = bind(request, false)
    private fun bind(request: DownloadRequest, start: Boolean) = operation {
        val token = NativeBridge.call(if (start) 2 else 3, handle, request.numbers(), request.texts(),
            request.provider?.let(::NativeProvider))[0].toLong()
        val sub = DownloadSubscription(this, token)
        tasks[token] = sub
        if (closing.get()) { sub.releaseNative(); throw FetchKitException(Status.Closed) }
        sub
    }
    /** 按稳定 persistentId 顺序分页；offset ≥ 0，capacity 为 1～100，不重新启动恢复任务。 */
    fun listTasks(offset: Int = 0, capacity: Int = 50): CompletableFuture<List<StoredTask>> {
        require(offset >= 0 && capacity in 1..100)
        return operation {
            NativeBridge.call(9, handle, longArrayOf(offset.toLong(), capacity.toLong())).chunked(25).map { row ->
                StoredTask(TaskSnapshot.decode(row.take(20)), ResourceIdentity(row[20], row[21], row[22], row[23]), row[24])
            }
        }
    }
    /** 查询逻辑字节计费，包含孤立/隔离文件与空间预留；不能替代文件系统剩余空间检查。 */
    fun storageStats(): CompletableFuture<StorageStats> = operation {
        val v = NativeBridge.call(10, handle)
        StorageStats(v[0].toInt(), v[1].toInt(), v[2].toInt(), v[3].toInt(), v[4].toLong(),
            v[5].toLong(), v[6].toLong(), v[7].toLong(), v[8].toLong())
    }
    /** 仅获取未过期且通过一致性校验的 Completed 对象；成功返回的 lease 须显式释放。 */
    fun acquire(persistentId: String): CompletableFuture<ResourceLease> = operation {
        ResourceLease(NativeBridge.call(11, handle, texts = arrayOf(persistentId))[0].toLong())
    }
    /** 持久化 SDK 淘汰保护；pin 不绕过过期/校验，也不能阻止系统清理缓存目录。 */
    fun pin(persistentId: String, pinned: Boolean): CompletableFuture<Unit> = operation {
        NativeBridge.call(12, handle, longArrayOf(if (pinned) 1 else 0), arrayOf(persistentId)); Unit
    }
    /** 设置 UTC Unix 毫秒过期时间，0 表示不过期；已持有的 lease 不受新过期时间影响。 */
    fun setExpiry(persistentId: String, expiresAtMs: Long): CompletableFuture<Unit> {
        require(expiresAtMs >= 0)
        return operation { NativeBridge.call(13, handle, longArrayOf(expiresAtMs), arrayOf(persistentId)); Unit }
    }
    /** 显式移除资源；活动任务、pin 或 lease 保护导致 Busy，须先停止任务/解除保护。 */
    fun remove(persistentId: String): CompletableFuture<Unit> = operation {
        NativeBridge.call(14, handle, texts = arrayOf(persistentId)); Unit
    }
    /** 按策略回收至目标逻辑字节（bytes ≥ 0）；保护中的资源不回收，无法达标时返回 QuotaExceeded。 */
    fun trimTo(bytes: Long): CompletableFuture<Unit> {
        require(bytes >= 0)
        return operation { NativeBridge.call(15, handle, longArrayOf(bytes)); Unit }
    }
    /** 清理超过保留期且无活动任务、pin 或 lease 保护的已记录暂存数据。 */
    fun collectStaging(): CompletableFuture<Unit> = operation { NativeBridge.call(16, handle); Unit }
    internal fun forget(token: Long) { tasks.remove(token) }

    /** 停止 I/O/provider 并释放 native 句柄后完成；旧事件失效，不等待宿主 executor，已有 lease 仍可读。 */
    fun closeAsync(): CompletableFuture<Unit> {
        if (closing.compareAndSet(false, true)) {
            tasks.values.forEach { it.suppress() }
            EngineIo.controls.execute {
                val lock = calls.writeLock()
                var error: Throwable? = null
                val closingTasks = tasks.values.toList()
                lock.lock()
                try {
                    NativeBridge.call(1, handle)
                    tasks.values.toList().forEach { it.releaseNative() }
                    transport.close()
                    pump.shutdown()
                    EngineIo.openSlots.release()
                } catch (failure: Throwable) { error = failure }
                finally { lock.unlock() }
                // Future 完成可能同步执行宿主代码，必须先解锁；native 已记录关闭失败，避免重复输出。
                if (error != null && error !is FetchKitException) SdkLog.error(DiagnosticEvent.EngineCleanupFailed, error)
                if (error == null) closeResult.complete(Unit) else closeResult.completeExceptionally(error)
                closingTasks.forEach { it.finishWaiters() }
            }
        }
        return closeResult
    }
}

/** 独立订阅句柄；taskId 为当前进程的订阅 token，跨进程身份请使用快照中的 persistentId。 */
class DownloadSubscription internal constructor(private val engine: DownloadEngine, val taskId: Long) {
    private val released = AtomicBoolean()
    private val suppressed = AtomicBoolean()
    private val observing = AtomicBoolean()
    private var observation: java.util.concurrent.ScheduledFuture<*>? = null
    private val releaseLock = Any()
    private val waits = java.util.concurrent.ConcurrentHashMap.newKeySet<CompletableFuture<TaskSnapshot>>()

    fun query(): CompletableFuture<TaskSnapshot> = engine.operation { queryNative() }
    private fun queryNative(): TaskSnapshot {
        if (released.get()) throw FetchKitException(Status.Closed)
        return engine.nativeRead { TaskSnapshot.decode(NativeBridge.call(4, taskId)) }
    }
    /** 恢复共享任务并创建新 generation，影响所有订阅；完成 Future 只表示控制请求已接受。 */
    fun resume(): CompletableFuture<Unit> = engine.operation { NativeBridge.call(6, taskId); Unit }
    /** 撤销本订阅；最后一个显式取消才清理共享未完成数据。返回快照不代表共享 I/O 已静止。 */
    fun cancel(): CompletableFuture<TaskSnapshot> = engine.operation { NativeBridge.call(7, taskId); queryNative() }
    /**
     * 请求暂停整个共享任务，并异步等待停止状态；仅返回 Paused 才确认检查点成功。
     * 竞争中的完成、失败或取消也会正常返回相应快照，调用方须检查 state/error。
     */
    fun pause(): CompletableFuture<TaskSnapshot> = engine.operation { NativeBridge.call(5, taskId); Unit }
        .thenCompose { awaitStopped() }
    /** 等待任一停止状态（包括失败/取消）；同一订阅复用未完成的等待，不占用控制线程阻塞轮询。 */
    @Synchronized fun awaitStopped(): CompletableFuture<TaskSnapshot> {
        waits.firstOrNull { !it.isDone }?.let { return it }
        val future = CompletableFuture<TaskSnapshot>()
        if (suppressed.get() || released.get()) {
            future.completeExceptionally(FetchKitException(Status.Closed)); return future
        }
        waits.add(future)
        val polling = try { engine.pump.scheduleWithFixedDelay({
            try { val value = queryNative(); if (value.state.stopped) future.complete(value) }
            catch (e: Throwable) { future.completeExceptionally(e) }
        }, 0, 25, TimeUnit.MILLISECONDS) } catch (_: java.util.concurrent.RejectedExecutionException) {
            waits.remove(future); future.completeExceptionally(FetchKitException(Status.Closed)); return future
        }
        if (suppressed.get()) future.completeExceptionally(FetchKitException(Status.Closed))
        future.whenComplete { _, _ -> polling.cancel(false); waits.remove(future) }
        return future
    }

    /**
     * 每个订阅生命周期只允许注册一次观察者，在宿主 executor 上顺序回调；最多一个批次等待分发。
     * listener 抛异常或 executor 拒绝时停止观察并请求取消此订阅；关闭返回的句柄仅停止观察，不释放订阅。
     */
    fun observe(executor: Executor, listener: (TaskEvent) -> Unit): AutoCloseable {
        check(observing.compareAndSet(false, true)) { "One observer per subscription" }
        val active = AtomicBoolean(true)
        val pending = AtomicBoolean()
        observation = engine.pump.scheduleWithFixedDelay({
            if (!active.get() || suppressed.get() || released.get() || !pending.compareAndSet(false, true)) return@scheduleWithFixedDelay
            try {
                val events = engine.nativeRead { NativeBridge.call(17, taskId).chunked(13).map { TaskEvent.decode(it) } }
                if (events.isEmpty()) pending.set(false)
                else executor.execute {
                    try { events.forEach { if (active.get() && !suppressed.get() && !released.get()) listener(it) } }
                    catch (failure: Throwable) {
                        active.set(false)
                        observationFailed(DiagnosticEvent.ListenerFailed, failure)
                    }
                    finally { pending.set(false) }
                }
            } catch (failure: Throwable) {
                active.set(false); pending.set(false)
                observationFailed(if (failure is java.util.concurrent.RejectedExecutionException)
                    DiagnosticEvent.ExecutorRejected else DiagnosticEvent.EventPumpFailed, failure)
            }
        }, 0, 50, TimeUnit.MILLISECONDS)
        return AutoCloseable { active.set(false); observation?.cancel(false) }
    }
    private fun observationFailed(event: DiagnosticEvent, failure: Throwable) {
        observation?.cancel(false)
        if (suppressed.get() || released.get()) return
        SdkLog.error(event, failure, taskId)
        // 取消也是异步操作；保留后续失败线索，避免把 Closed/Cancelled 等正常收尾记录为错误。
        cancel().whenComplete { _, error ->
            val cause = (error as? java.util.concurrent.CompletionException)?.cause ?: error
            if (cause != null && !suppressed.get() && !released.get() &&
                !(cause is FetchKitException && cause.status in setOf(Status.Closed, Status.Cancelled)))
                SdkLog.error(DiagnosticEvent.ObserverCancelFailed, cause, taskId)
        }
    }
    internal fun suppress() {
        suppressed.set(true); observation?.cancel(false)
    }
    internal fun finishWaiters() {
        waits.forEach { it.completeExceptionally(FetchKitException(Status.Closed)) }
    }
    internal fun releaseNative() = synchronized(releaseLock) {
        suppress()
        if (!released.get()) {
            NativeBridge.call(8, taskId)
            released.set(true)
            engine.forget(taskId)
        }
    }
    /** 静默解除订阅；最后一个释放会停止 I/O、尽力提交并保留暂停记录。完成后不得继续使用此句柄。 */
    fun releaseAsync(): CompletableFuture<Unit> = EngineIo.submit { releaseNative(); finishWaiters(); Unit }
}

/** 已发布对象的只读租约；持有期间阻止淘汰，并使 engine 关闭后目录锁继续有效。 */
class ResourceLease internal constructor(private val handle: Long) {
    private val released = AtomicBoolean()
    private val lock = Any()
    /** offset 为非负字节偏移，capacity 为 1～65536；EOF 返回空数组，engine 关闭后仍可读。 */
    fun read(offset: Long, capacity: Int = 65536): CompletableFuture<ByteArray> {
        require(offset >= 0 && capacity in 1..65536)
        return EngineIo.submit { synchronized(lock) {
            if (released.get()) throw FetchKitException(Status.Closed)
            val buffer = ByteArray(capacity)
            val count = NativeBridge.read(handle, offset, buffer)
            if (count < 0) throw FetchKitException(Status.entries[-count])
            buffer.copyOf(count)
        } }
    }
    /** 显式释放租约；最后一个 lease 释放后才可能解除已关闭 engine 的目录锁。重复释放安全。 */
    fun releaseAsync(): CompletableFuture<Unit> = EngineIo.submit { synchronized(lock) {
        if (!released.get()) { NativeBridge.call(18, handle); released.set(true) }
        Unit
    } }
}
