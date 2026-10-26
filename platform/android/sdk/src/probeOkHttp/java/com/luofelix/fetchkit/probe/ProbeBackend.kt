package com.luofelix.fetchkit.probe

import okhttp3.Authenticator
import okhttp3.ConnectionPool
import okhttp3.CookieJar
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import java.net.Proxy
import java.net.URL
import java.util.concurrent.TimeUnit

internal object ProbeBackend {
    const val name = "OkHttp 5.5.1 isolated M0 probe"
    private val client = OkHttpClient.Builder()
        .followRedirects(false)
        .followSslRedirects(false)
        .retryOnConnectionFailure(false)
        .cookieJar(CookieJar.NO_COOKIES)
        .authenticator(Authenticator.NONE)
        .proxyAuthenticator(Authenticator.NONE)
        .proxy(Proxy.NO_PROXY)
        .cache(null)
        .connectionPool(ConnectionPool(0, 1, TimeUnit.SECONDS))
        .connectTimeout(5, TimeUnit.SECONDS)
        .build()

    fun open(url: URL, headers: Map<String, String>, readTimeoutMs: Int): ProbeConnection {
        val request = Request.Builder().url(url)
            .header("Accept-Encoding", "identity")
            .header("Connection", "close")
        headers.forEach { (key, value) -> request.header(key, value) }
        val call = client.newBuilder().readTimeout(readTimeoutMs.toLong(), TimeUnit.MILLISECONDS)
            .build().newCall(request.build())
        return object : ProbeConnection {
            private var response: Response? = null
            override fun connect() { response = call.execute() }
            override val status get() = requireNotNull(response).code
            override val headers: List<Pair<String, String>>
                get() = requireNotNull(response).headers.let { h -> (0 until h.size).map { h.name(it) to h.value(it) } }
            override val length get() = requireNotNull(response).body.contentLength()
            override fun stream() = requireNotNull(response).body.byteStream()
            override fun cancel() = call.cancel()
            override fun close() { response?.close() }
        }
    }
}
