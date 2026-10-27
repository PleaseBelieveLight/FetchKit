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

/** All disk/control operations are asynchronous. Cancelling a future does not cancel a download. */
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
    fun enqueue(request: DownloadRequest): CompletableFuture<DownloadSubscription> = bind(request, true)
    fun attach(request: DownloadRequest): CompletableFuture<DownloadSubscription> = bind(request, false)
    private fun bind(request: DownloadRequest, start: Boolean) = operation {
        val token = NativeBridge.call(if (start) 2 else 3, handle, request.numbers(), request.texts(),
            request.provider?.let(::NativeProvider))[0].toLong()
        val sub = DownloadSubscription(this, token)
        tasks[token] = sub
        if (closing.get()) { sub.releaseNative(); throw FetchKitException(Status.Closed) }
        sub
    }
    fun listTasks(offset: Int = 0, capacity: Int = 50): CompletableFuture<List<StoredTask>> {
        require(offset >= 0 && capacity in 1..100)
        return operation {
            NativeBridge.call(9, handle, longArrayOf(offset.toLong(), capacity.toLong())).chunked(25).map { row ->
                StoredTask(TaskSnapshot.decode(row.take(20)), ResourceIdentity(row[20], row[21], row[22], row[23]), row[24])
            }
        }
    }
    fun storageStats(): CompletableFuture<StorageStats> = operation {
        val v = NativeBridge.call(10, handle)
        StorageStats(v[0].toInt(), v[1].toInt(), v[2].toInt(), v[3].toInt(), v[4].toLong(),
            v[5].toLong(), v[6].toLong(), v[7].toLong(), v[8].toLong())
    }
    fun acquire(persistentId: String): CompletableFuture<ResourceLease> = operation {
        ResourceLease(NativeBridge.call(11, handle, texts = arrayOf(persistentId))[0].toLong())
    }
    fun pin(persistentId: String, pinned: Boolean): CompletableFuture<Unit> = operation {
        NativeBridge.call(12, handle, longArrayOf(if (pinned) 1 else 0), arrayOf(persistentId)); Unit
    }
    fun setExpiry(persistentId: String, expiresAtMs: Long): CompletableFuture<Unit> {
        require(expiresAtMs >= 0)
        return operation { NativeBridge.call(13, handle, longArrayOf(expiresAtMs), arrayOf(persistentId)); Unit }
    }
    fun remove(persistentId: String): CompletableFuture<Unit> = operation {
        NativeBridge.call(14, handle, texts = arrayOf(persistentId)); Unit
    }
    fun trimTo(bytes: Long): CompletableFuture<Unit> {
        require(bytes >= 0)
        return operation { NativeBridge.call(15, handle, longArrayOf(bytes)); Unit }
    }
    fun collectStaging(): CompletableFuture<Unit> = operation { NativeBridge.call(16, handle); Unit }
    internal fun forget(token: Long) { tasks.remove(token) }

    /** Completes after I/O/provider quiescence and native handles are released. Existing leases survive. */
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
                // CompletableFuture completion may run host code inline. No SDK lock is held.
                if (error == null) closeResult.complete(Unit) else closeResult.completeExceptionally(error)
                closingTasks.forEach { it.finishWaiters() }
            }
        }
        return closeResult
    }
}

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
    fun resume(): CompletableFuture<Unit> = engine.operation { NativeBridge.call(6, taskId); Unit }
    fun cancel(): CompletableFuture<TaskSnapshot> = engine.operation { NativeBridge.call(7, taskId); queryNative() }
    /** Waits asynchronously for the Paused checkpoint; does not occupy a control thread while waiting. */
    fun pause(): CompletableFuture<TaskSnapshot> = engine.operation { NativeBridge.call(5, taskId); Unit }
        .thenCompose { awaitStopped() }
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

    /** One batch may wait on the executor; the native bounded queue retains backpressure. */
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
                    catch (_: Throwable) { active.set(false); cancel() }
                    finally { pending.set(false) }
                }
            } catch (_: Throwable) { active.set(false); pending.set(false); cancel() }
        }, 0, 50, TimeUnit.MILLISECONDS)
        return AutoCloseable { active.set(false); observation?.cancel(false) }
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
    fun releaseAsync(): CompletableFuture<Unit> = EngineIo.submit { releaseNative(); finishWaiters(); Unit }
}

class ResourceLease internal constructor(private val handle: Long) {
    private val released = AtomicBoolean()
    private val lock = Any()
    /** Bounded reads remain available after engine close. Call repeatedly to read a larger object. */
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
    fun releaseAsync(): CompletableFuture<Unit> = EngineIo.submit { synchronized(lock) {
        if (!released.get()) { NativeBridge.call(18, handle); released.set(true) }
        Unit
    } }
}
