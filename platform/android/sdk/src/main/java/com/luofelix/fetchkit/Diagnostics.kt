package com.luofelix.fetchkit

import android.util.Log

/**
 * SDK 诊断级别；默认 [Off]，每个级别包含更严重的消息，Debug 不包含逐进度或逐数据块日志。
 * 枚举顺序与 native 的 FETCHKIT_LOG_* 一致，不能单独重排。
 */
enum class LogLevel {
    Off, Error, Warn, Debug;

    internal fun includes(messageLevel: LogLevel): Boolean = messageLevel != Off && ordinal >= messageLevel.ordinal
}

// 固定文案与类型化字段构成白名单；调用方不能把 URL、headers 或异常消息拼进日志。
internal enum class DiagnosticEvent(val text: String) {
    TransportInitFailed("网络适配器初始化失败"),
    EngineCleanupFailed("引擎资源清理失败"),
    ProviderFailed("请求 provider 执行失败"),
    ListenerFailed("订阅 listener 异常，已停止观察并请求取消订阅"),
    ExecutorRejected("宿主 executor 拒绝事件分发，已停止观察并请求取消订阅"),
    EventPumpFailed("订阅事件读取或解码失败，已停止观察并请求取消订阅"),
    ObserverCancelFailed("观察异常后的订阅取消失败"),
}

internal fun diagnosticMessage(event: DiagnosticEvent, failure: Throwable, taskId: Long? = null,
    generation: Long? = null): String = buildString {
    append(event.text)
    taskId?.let { append(" taskId=").append(it) }
    generation?.let { append(" generation=").append(it) }
    if (failure is FetchKitException) append(" status=").append(failure.status)
    // 只记录异常类型，绝不访问 message、cause 或 stackTrace 中的外部字符串。
    append(" exception=").append(failure.javaClass.simpleName.take(80))
}

internal object SdkLog {
    @Volatile private var level = LogLevel.Off

    @Synchronized fun configure(value: LogLevel) {
        NativeBridge.call(19, numbers = longArrayOf(value.ordinal.toLong()))
        level = value
    }

    fun error(event: DiagnosticEvent, failure: Throwable, taskId: Long? = null, generation: Long? = null) {
        if (!level.includes(LogLevel.Error)) return
        try {
            Log.println(Log.ERROR, "FetchKit", diagnosticMessage(event, failure, taskId, generation))
        } catch (_: Throwable) {
            // 诊断设施失败不能覆盖业务错误，也不能触发递归日志或重试。
        }
    }
}
