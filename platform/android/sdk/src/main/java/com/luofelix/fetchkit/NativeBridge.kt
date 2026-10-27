package com.luofelix.fetchkit

internal object NativeBridge {
    external fun invoke(operation: Int, handle: Long, numbers: LongArray, texts: Array<String>,
        provider: NativeProvider?, transport: AndroidTransport?): Array<String>
    external fun cancelled(operation: Long): Boolean
    external fun headers(operation: Long, status: Int, headers: Array<String>, url: String): Boolean
    external fun data(operation: Long, data: ByteArray, count: Int): Boolean
    external fun providerCancelled(scope: Long): Boolean
    external fun read(lease: Long, offset: Long, buffer: ByteArray): Int

    fun call(operation: Int, handle: Long = 0, numbers: LongArray = longArrayOf(),
        texts: Array<String> = emptyArray(), provider: NativeProvider? = null, transport: AndroidTransport? = null): List<String> {
        val result = invoke(operation, handle, numbers, texts, provider, transport)
        val status = Status.decode(result[0])
        if (status != Status.Ok) throw FetchKitException(status)
        return result.drop(1)
    }
}

internal class NativeProvider(private val provider: RequestProvider) {
    // Called only by a native worker; scoped cancellation tokens cannot outlive this call.
    fun provide(scope: Long, reason: Int, task: Long, generation: Long, status: Int,
        url: String, origin: String, reference: String): Array<String> {
        val value = provider.provide(RequestContext(RequestReason.entries[reason], task, generation, status,
            url, origin, reference) { NativeBridge.providerCancelled(scope) })
        return arrayOf(value.url) + value.headers.flatMap { listOf(it.key, it.value) }
    }
}
