package com.luofelix.fetchkit

/** Entry point for the FetchKit Android SDK. */
object FetchKit {
    init {
        System.loadLibrary("fetchkit_jni")
    }

    /** Returns the version of the linked C++ core. */
    fun version(): String = nativeVersion()

    /** Opens and repairs storage off the caller thread. Supply a private, backup-excluded root. */
    fun open(config: EngineConfig): java.util.concurrent.CompletableFuture<DownloadEngine> = EngineIo.submit {
        if (!EngineIo.openSlots.tryAcquire()) throw FetchKitException(Status.Busy)
        val transport = try { AndroidTransport(config.network) } catch (error: Throwable) { EngineIo.openSlots.release(); throw error }
        try {
            val handle = NativeBridge.call(0, numbers = config.numbers(),
                texts = arrayOf(config.storageRoot.absolutePath), transport = transport)[0].toLong()
            DownloadEngine(handle, transport)
        } catch (error: Throwable) { transport.close(); EngineIo.openSlots.release(); throw error }
    }

    private external fun nativeVersion(): String
}
