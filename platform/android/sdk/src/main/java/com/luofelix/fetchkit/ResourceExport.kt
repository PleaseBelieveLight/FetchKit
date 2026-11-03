package com.luofelix.fetchkit

import java.io.IOException
import java.io.OutputStream
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.CompletableFuture
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/** 一次导出操作。取消只影响复制，不撤销下载订阅。 */
class ResourceExport private constructor() {
    private val cancelled = AtomicBoolean()
    private val result = CompletableFuture<Long>()

    /**
     * 在关闭 SDK 打开的目标并释放租约后完成，成功值为写入字节数。
     * 取消这个 Future 的副本只停止等待；请通过 [cancel] 请求停止复制。
     */
    val completion: CompletableFuture<Long> get() = result.thenApply { it }

    /** 请求在下一次读写边界停止。正在阻塞的目标 I/O 必须先返回，成功与取消竞争时可能已经写完。 */
    fun cancel() { cancelled.set(true) }

    private fun checkCancelled() {
        if (cancelled.get()) throw FetchKitException(Status.Cancelled)
    }

    private fun copy(lease: ResourceLease, output: OutputStream): Long {
        val buffer = ByteArray(65536)
        var offset = 0L
        while (true) {
            checkCancelled()
            val count = lease.readNative(offset, buffer)
            if (count == 0) return offset
            checkCancelled()
            output.write(buffer, 0, count)
            offset += count
        }
    }

    companion object {
        // 慢目标不占用 API 工作线程。排队任务在执行前不持有租约或目标文件。
        private val worker = ThreadPoolExecutor(1, 1, 30, TimeUnit.SECONDS, ArrayBlockingQueue(8),
            { Thread(it, "fetchkit-export").apply { isDaemon = true } }, ThreadPoolExecutor.AbortPolicy())

        internal fun start(
            acquire: () -> ResourceLease,
            destination: ((OutputStream) -> Long) -> Long,
        ): ResourceExport {
            val export = ResourceExport()
            try {
                worker.execute {
                    try {
                        export.checkCancelled()
                        val lease = acquire()
                        val count = try {
                            export.checkCancelled()
                            destination { export.copy(lease, it) }
                        } finally {
                            lease.releaseNative()
                        }
                        export.checkCancelled()
                        export.result.complete(count)
                    } catch (error: Throwable) {
                        export.result.completeExceptionally(when (error) {
                            is SecurityException -> FetchKitException(Status.PermissionDenied)
                            is IOException -> FetchKitException(Status.IoError)
                            is IllegalArgumentException -> FetchKitException(Status.InvalidArgument)
                            else -> error
                        })
                    }
                }
            } catch (_: RejectedExecutionException) {
                export.result.completeExceptionally(FetchKitException(Status.Busy))
            }
            return export
        }
    }
}
