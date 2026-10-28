package com.luofelix.fetchkit

import okhttp3.Authenticator
import okhttp3.ConnectionPool
import okhttp3.CookieJar
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.OkHttpClient
import okhttp3.Request
import java.net.Proxy
import java.net.SocketTimeoutException
import java.net.ProtocolException
import java.io.IOException
import java.io.InterruptedIOException
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import javax.net.ssl.SSLException
import javax.net.ssl.SSLPeerUnverifiedException
import javax.net.ssl.SSLHandshakeException
import java.security.cert.CertificateException

/** 每个 engine 使用隔离的客户端；重试、鉴权、重定向与续传策略统一由 C++ 核心决定。 */
internal class AndroidTransport(private val network: android.net.Network? = null) : AutoCloseable {
    private val cancellations = ScheduledThreadPoolExecutor(1) { Thread(it, "fetchkit-cancel").apply { isDaemon = true } }
        .apply { removeOnCancelPolicy = true }
    private val client = OkHttpClient.Builder().followRedirects(false).followSslRedirects(false)
        .retryOnConnectionFailure(false).cookieJar(CookieJar.NO_COOKIES)
        .authenticator(Authenticator.NONE).proxyAuthenticator(Authenticator.NONE).proxy(Proxy.NO_PROXY)
        .cache(null).connectionPool(ConnectionPool(0, 1, TimeUnit.SECONDS))
        .apply { if (network != null) socketFactory(network.socketFactory) }.build()

    fun resolve(base: String, location: String): Array<String> {
        val url = (if (base.isEmpty()) location.toHttpUrlOrNull() else base.toHttpUrlOrNull()?.resolve(location))
            ?: throw IllegalArgumentException("Invalid HTTP URL")
        require(url.username.isEmpty() && url.password.isEmpty())
        val canonical = url.newBuilder().fragment(null).build()
        val host = if (canonical.host.contains(':')) "[${canonical.host}]" else canonical.host
        return arrayOf(canonical.toString(), "${canonical.scheme}://$host:${canonical.port}",
            if (canonical.isHttps) "1" else "0")
    }

    // 低 8 位对应 transport::EndReason，高位为平台诊断分类；不回传 Java 异常消息。
    fun execute(operation: Long, url: String, headers: Array<String>, connectMs: Int, readMs: Int): Int {
        val request = Request.Builder().url(url)
        val requestHeaders = okhttp3.Headers.Builder()
        // 核心已校验控制字符、CR/LF、非法字段名和 SDK 保留字段。
        headers.asList().chunked(2).forEach { requestHeaders.addUnsafeNonAscii(it[0], it[1]) }
        request.headers(requestHeaders.build())
        request.header("Accept-Encoding", "identity")
        request.header("Connection", "close")
        var originalStatus = 0
        var accepted = false
        var declared = -1L
        var received = 0L
        val headersDone = AtomicBoolean()
        val timedOut = AtomicBoolean()
        val deadline = AtomicLong(System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(connectMs.toLong()))
        val call = client.newBuilder().connectTimeout(connectMs.toLong(), TimeUnit.MILLISECONDS)
            .readTimeout(readMs.toLong(), TimeUnit.MILLISECONDS)
            .dns { host -> BoundedDns.lookup(host, connectMs.toLong(), { NativeBridge.cancelled(operation) }) {
                if (network == null) java.net.InetAddress.getAllByName(it).toList() else network.getAllByName(it).toList()
            } }
            .eventListener(object : okhttp3.EventListener() {
                override fun connectionAcquired(call: okhttp3.Call, connection: okhttp3.Connection) {
                    deadline.set(System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(readMs.toLong()))
                }
            })
            // OkHttp 5.5.1 关闭连接重试后仍可能跟进 503 + Retry-After:0。
            // 在策略层前暂存真实状态并替换为 599，避免绕过核心重试预算；headers/body 保持原样。
            .addNetworkInterceptor { chain ->
                val response = chain.proceed(chain.request())
                originalStatus = response.code
                response.newBuilder().code(599).build()
            }.build().newCall(request.build())
        val gate = Any()
        var finished = false
        val monitor = cancellations.scheduleWithFixedDelay({
            synchronized(gate) {
                if (!finished) {
                    if (NativeBridge.cancelled(operation)) call.cancel()
                    else if (!headersDone.get() && System.nanoTime() - deadline.get() >= 0) {
                        timedOut.set(true); call.cancel()
                    }
                }
            }
        }, 0, 25, TimeUnit.MILLISECONDS)
        try {
            if (NativeBridge.cancelled(operation)) return 1
            call.execute().use { response ->
                headersDone.set(true)
                val fields = ArrayList<String>()
                var bytes = 0
                for (i in 0 until response.headers.size) {
                    val name = response.headers.name(i)
                    val value = response.headers.value(i)
                    // OkHttp 仅提供解码后的字符串；保守拒绝非 ASCII ETag，避免不同原始值折叠后错误追加。
                    if (name.equals("ETag", true) && value.any { it.code > 127 }) return 5
                    bytes += name.length + value.length + 4
                    if (i >= 256 || bytes > 65536) return 5
                    fields += name; fields += value
                }
                if (!NativeBridge.headers(operation, originalStatus, fields.toTypedArray(), response.request.url.toString())) return 4
                accepted = true
                declared = response.body.contentLength()
                val buffer = ByteArray(65536)
                response.body.byteStream().use { input ->
                    while (true) {
                        if (NativeBridge.cancelled(operation)) return 1
                        val count = input.read(buffer)
                        if (count < 0) break
                        if (count != 0 && !NativeBridge.data(operation, buffer, count)) return 4
                        received += count
                    }
                }
            }
            return if (NativeBridge.cancelled(operation)) 1 else if (timedOut.get()) 2 else 0
        } catch (_: SocketTimeoutException) { return if (NativeBridge.cancelled(operation)) 1 else 2 or (1 shl 8)
        } catch (_: SSLPeerUnverifiedException) { return 6 or (2 shl 8)
        } catch (error: SSLHandshakeException) {
            val certificate = generateSequence<Throwable>(error) { it.cause }.take(16).any { it is CertificateException }
            return (if (timedOut.get()) 2 else if (certificate) 6 else 3) or (3 shl 8)
        } catch (_: SSLException) { return (if (NativeBridge.cancelled(operation)) 1 else if (timedOut.get()) 2 else 3) or (3 shl 8)
        } catch (_: ProtocolException) { return (if (NativeBridge.cancelled(operation)) 1 else if (timedOut.get()) 2 else if (accepted && declared >= 0 && received < declared) 3 else 5) or (4 shl 8)
        } catch (_: InterruptedIOException) { return if (NativeBridge.cancelled(operation)) 1 else (if (timedOut.get()) 2 else 3) or (5 shl 8)
        } catch (_: IOException) { return if (NativeBridge.cancelled(operation)) 1 else (if (timedOut.get()) 2 else 3) or (6 shl 8)
        } catch (_: Exception) { return if (NativeBridge.cancelled(operation)) 1 else (if (timedOut.get()) 2 else 3) or (7 shl 8)
        } finally {
            monitor.cancel(false)
            synchronized(gate) { finished = true } // 等待已进入的取消检查完成，返回后不再访问 native operation。
            call.cancel()
        }
    }

    override fun close() {
        cancellations.shutdownNow()
        client.connectionPool.evictAll()
        client.dispatcher.executorService.shutdown()
    }
}
