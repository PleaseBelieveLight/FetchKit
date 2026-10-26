package com.luofelix.fetchkit.probe

import java.net.HttpURLConnection
import java.net.Proxy
import java.net.URL

internal object ProbeBackend {
    const val name = "HttpURLConnection M0 probe"

    fun open(url: URL, headers: Map<String, String>, readTimeoutMs: Int): ProbeConnection {
        val connection = url.openConnection(Proxy.NO_PROXY) as HttpURLConnection
        connection.instanceFollowRedirects = false
        connection.useCaches = false
        connection.connectTimeout = 5000
        connection.readTimeout = readTimeoutMs
        connection.setRequestProperty("Accept-Encoding", "identity")
        connection.setRequestProperty("Connection", "close")
        headers.forEach { (key, value) -> connection.setRequestProperty(key, value) }
        return object : ProbeConnection {
            override var status = 0
            override fun connect() { status = connection.responseCode }
            override val headers: List<Pair<String, String>>
                get() = buildList {
                    for (index in 1..257) {
                        val key = connection.getHeaderFieldKey(index)
                        val value = connection.getHeaderField(index)
                        if (key == null && value == null) break
                        add((key ?: "") to (value ?: ""))
                    }
                }
            override val length get() = connection.contentLengthLong
            override fun stream() = if (status >= 400) connection.errorStream else connection.inputStream
            override fun cancel() = connection.disconnect()
            override fun close() = connection.disconnect()
        }
    }
}
