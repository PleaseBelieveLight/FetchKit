package com.luofelix.fetchkit.probe

import com.luofelix.fetchkit.FetchKit
import java.io.InputStream
import java.net.SocketTimeoutException
import java.net.URL
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/** Opt-in M0 experiment, excluded from normal AARs. This is not the download API. */
object PlatformProbe {
    init { FetchKit.version() }

    fun backendName(): String = ProbeBackend.name

    private external fun nativeFingerprint(bytes: ByteArray, count: Int, seed: Long): Long

    data class Result(
        var reason: String = "network",
        var status: Int = 0,
        var bytes: Long = 0,
        var fingerprint: Long = -3750763034362895579L,
        var headerCalls: Int = 0,
        var bodyCalls: Int = 0,
        var duplicateHeaders: Int = 0,
        var encoded: Boolean = false,
        var errorType: String = ""
    )

    class Operation(
        private val url: String,
        private val scenario: String,
        private val requestHeaders: Map<String, String> = emptyMap()
    ) {
        val headersSeen = CountDownLatch(1)
        val bodySeen = CountDownLatch(1)
        private val cancelled = AtomicBoolean(false)
        private val wake = CountDownLatch(1)
        private val connection = AtomicReference<ProbeConnection?>()
        private val stream = AtomicReference<InputStream?>()
        private var cleanupThread: Thread? = null

        @Synchronized
        fun cancel() {
            if (!cancelled.compareAndSet(false, true)) return
            wake.countDown()
            // Neither disconnect nor close is assumed non-blocking. The measurement
            // includes joining this cleanup thread as well as the network worker.
            cleanupThread = Thread({
                runCatching { connection.get()?.cancel() }
                runCatching { stream.get()?.close() }
            }, "fetchkit-probe-cancel").also { it.start() }
        }

        @Synchronized
        fun awaitCleanup(timeoutMs: Long): Boolean {
            cleanupThread?.join(timeoutMs)
            return cleanupThread?.isAlive != true
        }

        fun execute(): Result {
            val result = Result()
            if (cancelled.get()) return result.apply { reason = "cancelled" }
            try {
                val target = URL(url)
                require(target.protocol == "http" || target.protocol == "https")
                require(target.userInfo == null)
                val current = ProbeBackend.open(target, requestHeaders, if (scenario == "timeout") 250 else 2000)
                connection.set(current)
                if (cancelled.get()) return result.apply { reason = "cancelled" }
                current.connect()
                result.status = current.status
                var headerBytes = 0
                val responseHeaders = current.headers
                require(responseHeaders.size <= 256)
                for ((name, value) in responseHeaders) {
                    headerBytes += name.length + value.length
                    require(headerBytes <= 65536)
                    if (name.equals("X-FetchKit", true)) result.duplicateHeaders++
                    if (name.equals("Content-Encoding", true) && !value.equals("identity", true)) {
                        result.encoded = true
                    }
                }
                result.headerCalls++
                headersSeen.countDown()
                if (scenario == "header-barrier") wake.await(10, TimeUnit.SECONDS)
                if (cancelled.get()) return result.apply { reason = "cancelled" }
                if (scenario == "reject" || result.encoded) return result.apply { reason = "rejected" }
                val input = current.stream()
                stream.set(input)
                val buffer = ByteArray(65536)
                if (input != null) {
                    while (!cancelled.get()) {
                        val count = input.read(buffer)
                        if (count == -1) break
                        if (count == 0) continue
                        result.bodyCalls++
                        if (scenario == "backpressure") {
                            bodySeen.countDown()
                            wake.await(10, TimeUnit.SECONDS)
                            if (cancelled.get()) break
                        }
                        result.fingerprint = nativeFingerprint(buffer, count, result.fingerprint)
                        result.bytes += count
                        bodySeen.countDown()
                    }
                }
                val length = current.length
                result.reason = when {
                    cancelled.get() -> "cancelled"
                    length >= 0 && result.bytes != length -> "network"
                    else -> "complete"
                }
            } catch (error: Exception) {
                result.reason = when {
                    cancelled.get() -> "cancelled"
                    error is SocketTimeoutException -> "timeout"
                    else -> "network"
                }
                result.errorType = error.javaClass.simpleName // Never log exception messages/URLs.
            } finally {
                runCatching { stream.getAndSet(null)?.close() }
                runCatching { connection.getAndSet(null)?.close() }
            }
            return result
        }
    }
}

internal interface ProbeConnection {
    fun connect()
    val status: Int
    val headers: List<Pair<String, String>>
    val length: Long
    fun stream(): InputStream?
    fun cancel()
    fun close()
}
