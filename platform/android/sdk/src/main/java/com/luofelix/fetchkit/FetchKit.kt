package com.luofelix.fetchkit

/** FetchKit Android SDK 入口。 */
object FetchKit {
    init {
        System.loadLibrary("fetchkit_jni")
    }

    /** 返回实际链接的 C++ 核心版本。 */
    fun version(): String = nativeVersion()

    /**
     * 设置当前 SDK 实例内的进程级诊断日志级别，同时控制 Kotlin 与 C++；默认 [LogLevel.Off]。
     * 线程安全，立即影响后续日志，已进入输出的日志可能完成；关闭日志不影响错误、快照或事件。
     * 日志输出到 Logcat，tag 为 FetchKit；仅记录固定中文文案、安全标识和错误类型。
     */
    fun setLogLevel(level: LogLevel) = SdkLog.configure(level)

    /** 异步打开并扫描修复存储；根目录须由宿主私有使用并排除备份。同目录被占用时返回 Busy。 */
    fun open(config: EngineConfig): java.util.concurrent.CompletableFuture<DownloadEngine> = EngineIo.submit {
        if (!EngineIo.openSlots.tryAcquire()) throw FetchKitException(Status.Busy)
        val transport = try { AndroidTransport(config.network) } catch (error: Throwable) {
            EngineIo.openSlots.release()
            SdkLog.error(DiagnosticEvent.TransportInitFailed, error)
            throw error
        }
        try {
            val handle = NativeBridge.call(0, numbers = config.numbers(),
                texts = arrayOf(config.storageRoot.absolutePath), transport = transport)[0].toLong()
            DownloadEngine(handle, transport)
        } catch (error: Throwable) { transport.close(); EngineIo.openSlots.release(); throw error }
    }

    private external fun nativeVersion(): String
}
