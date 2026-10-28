package com.luofelix.fetchkit

internal object NativeBridge {
    // 仅桥接有界值数组；操作编号和字段顺序须与 android_bridge.cpp::invoke 同步，不作为公共 API 暴露。
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
    // 仅由 native worker 调用；取消 token 只在本次 provide 范围内有效。
    fun provide(scope: Long, reason: Int, task: Long, generation: Long, status: Int,
        url: String, origin: String, reference: String): Array<String> {
        val value = try {
            provider.provide(RequestContext(RequestReason.entries[reason], task, generation, status,
                url, origin, reference) { NativeBridge.providerCancelled(scope) })
        } catch (failure: Throwable) {
            if (!NativeBridge.providerCancelled(scope)) SdkLog.error(DiagnosticEvent.ProviderFailed, failure, task, generation)
            throw failure // JNI 保留原有 AuthRequired 映射，日志不改变 provider 契约。
        }
        return arrayOf(value.url) + value.headers.flatMap { listOf(it.key, it.value) }
    }
}
