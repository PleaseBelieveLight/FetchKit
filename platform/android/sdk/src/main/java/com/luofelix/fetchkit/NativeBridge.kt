package com.luofelix.fetchkit

/** SDK 内部的同步 JNI 桥接。涉及磁盘 I/O 或等待 native 操作的调用由 SDK 工作线程执行。 */
internal object NativeBridge {
    /**
     * 按内部协议执行一次 native 操作。编号、字段顺序和单位须与
     * `android_bridge.cpp` 中的 `invoke` 对应分支保持一致。
     *
     * @param operation 操作编号。
     * @param handle 对应操作所需的引擎、订阅或读取租约句柄，无需句柄时传 0。
     * @param numbers 位置固定的非负整数参数，最多 16 项。
     * @param texts 位置固定的字符串参数，最多 512 项。转为 UTF-8 后单项最多 16 KiB、
     * 总计最多 1 MiB，拒绝 NUL 和孤立代理项。各操作还可限制字段数量和值。
     * @param provider 入队、绑定或重试时使用的可选请求提供者，native 可持有其引用供后续回调。
     * @param transport 打开引擎时必传的 Android 传输后端，由引擎持有。
     * @return 首项为十进制状态码（0 表示成功），后续为该操作的结果字段。
     * 成功返回新句柄时，调用方负责对应的关闭或释放。
     */
    external fun invoke(
        operation: Int, handle: Long, numbers: LongArray, texts: Array<String>,
        provider: NativeProvider?, transport: AndroidTransport?
    ): Array<String>

    /**
     * 查询本轮 HTTP 传输是否应停止。
     *
     * @param operation 本次 [AndroidTransport.execute] 收到的传输令牌。
     * @return 传输已取消、令牌已失效或查询失败时返回 `true`。
     */
    external fun cancelled(operation: Long): Boolean

    /**
     * 将响应头交给核心判定。只有返回 `true` 后才可调用 [data] 交付响应体。
     *
     * @param operation 本次 [AndroidTransport.execute] 收到的传输令牌。
     * @param status 原始 HTTP 响应状态码。
     * @param headers 按名称、值交替排列，最多 256 对。按 UTF-8 计，每对的名称与值
     * 再加 4 字节开销，合计不得超过 64 KiB。
     * @param url 实际发出请求的 URL。
     * @return 核心接受时返回 `true`。拒绝、令牌失效或桥接失败时返回 `false`。
     */
    external fun headers(operation: Long, status: Int, headers: Array<String>, url: String): Boolean

    /**
     * 同步复制并交付一块响应体，返回后调用方可复用缓冲区。
     *
     * @param operation 响应头已被接受的本轮传输令牌。
     * @param data 从下标 0 开始存放本块数据的缓冲区。
     * @param count 有效字节数，至少为 0，且不得超过缓冲区长度或 65536。
     * @return 核心接受时返回 `true`。拒绝、令牌失效或桥接失败时返回 `false`，
     * 调用方应停止交付后续数据。
     */
    external fun data(operation: Long, data: ByteArray, count: Int): Boolean

    /**
     * 查询本次 provider 回调是否应协作停止。
     *
     * @param scope [NativeProvider.provide] 收到的作用域令牌。
     * @return 回调已取消或作用域已失效时返回 `true`。
     */
    external fun providerCancelled(scope: Long): Boolean

    /**
     * 从已获取的读取租约同步读取数据。租约在引擎关闭后仍可用，调用方须在读完后释放。
     *
     * @param lease 尚未释放的读取租约句柄。
     * @param offset 从文件开头计算的非负字节偏移。
     * @param buffer 长度为 1～65536 字节的目标缓冲区，结果从下标 0 写入。
     * @return 实际读取的字节数，0 表示无数据可读。失败时返回 SDK 状态码的负值。
     */
    external fun read(lease: Long, offset: Long, buffer: ByteArray): Int

    /**
     * 调用 [invoke] 并解码首项状态码。参数沿用该操作的 JNI 字段布局。
     *
     * @return 成功时返回去掉状态码后的字段列表，无结果字段时返回空列表。
     * @throws FetchKitException native 返回非 [Status.Ok] 状态时抛出。
     */
    fun call(
        operation: Int,
        handle: Long = 0,
        numbers: LongArray = longArrayOf(),
        texts: Array<String> = emptyArray(),
        provider: NativeProvider? = null,
        transport: AndroidTransport? = null
    ): List<String> {
        val result = invoke(operation, handle, numbers, texts, provider, transport)
        val status = Status.decode(result[0])
        if (status != Status.Ok) throw FetchKitException(status)
        return result.drop(1)
    }
}

/** 将宿主 [RequestProvider] 适配为 native 按名称调用的 JNI 回调。 */
internal class NativeProvider(private val provider: RequestProvider) {
    /**
     * 在 native worker 上同步获取本轮请求。provider 须协作检查取消，
     * 并避免阻塞等待引擎 API 的 Future。
     *
     * @param scope 仅在本次回调作用域内有效的取消令牌。
     * @param reason [RequestReason] 的序号。
     * @param task 本次回调关联的任务 ID。
     * @param generation 当前运行代次。
     * @param status 触发回调的 HTTP 状态码，初始请求时为 0。
     * @param url 核心建议使用的请求 URL。
     * @param origin 上一请求的源，初始请求时为空字符串。
     * @param reference 宿主提供的业务地址引用。
     * @return 成功时依次返回 `"0"`、URL 和交替排列的 header 名称与值。
     * [RequestProviderException] 转为只含状态码的数组，其他异常由 JNI 映射为
     * [Status.AuthRequired]。
     */
    fun provide(
        scope: Long, reason: Int, task: Long, generation: Long, status: Int,
        url: String, origin: String, reference: String
    ): Array<String> {
        val value = try {
            provider.provide(
                RequestContext(
                    RequestReason.entries[reason], task, generation, status,
                    url, origin, reference
                ) { NativeBridge.providerCancelled(scope) })
        } catch (failure: Throwable) {
            if (failure is RequestProviderException) return arrayOf(failure.status.ordinal.toString())
            if (!NativeBridge.providerCancelled(scope)) SdkLog.error(
                DiagnosticEvent.ProviderFailed,
                failure,
                task,
                generation
            )
            throw failure // JNI 保留原有 AuthRequired 映射，日志不改变 provider 契约。
        }
        return arrayOf("0", value.url) + value.headers.flatMap { listOf(it.key, it.value) }
    }
}
