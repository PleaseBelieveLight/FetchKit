package com.luofelix.fetchkit

import java.io.InterruptedIOException
import java.net.InetAddress
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.ExecutionException
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException

/** A platform resolver may ignore interrupts. Its bounded workers never retain JNI/sink/provider state. */
internal object BoundedDns {
    private val pool = ThreadPoolExecutor(4, 4, 30, TimeUnit.SECONDS, ArrayBlockingQueue(64),
        { Thread(it, "fetchkit-dns").apply { isDaemon = true } }, ThreadPoolExecutor.AbortPolicy())
        .apply { allowCoreThreadTimeOut(true) }

    fun lookup(host: String, timeoutMs: Long, cancelled: () -> Boolean,
        resolve: (String) -> List<InetAddress> = { InetAddress.getAllByName(it).toList() }): List<InetAddress> {
        val future = try { pool.submit<List<InetAddress>> { resolve(host) } }
        catch (_: java.util.concurrent.RejectedExecutionException) { throw UnknownHostException("Resolver capacity") }
        val started = System.nanoTime()
        try {
            while (true) {
                if (cancelled()) throw InterruptedIOException("DNS cancelled")
                val remaining = TimeUnit.MILLISECONDS.toNanos(timeoutMs) - (System.nanoTime() - started)
                if (remaining <= 0) throw SocketTimeoutException("DNS deadline")
                try { return future.get(minOf(remaining, TimeUnit.MILLISECONDS.toNanos(25)), TimeUnit.NANOSECONDS) }
                catch (_: TimeoutException) {}
            }
        } catch (error: ExecutionException) {
            throw (error.cause as? java.io.IOException ?: UnknownHostException("Resolver failed"))
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt(); throw InterruptedIOException("DNS interrupted")
        } finally {
            if (!future.isDone) future.cancel(true)
            pool.remove(future as Runnable)
        }
    }
}
