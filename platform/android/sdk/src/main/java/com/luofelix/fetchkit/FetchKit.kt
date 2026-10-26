package com.luofelix.fetchkit

/** Entry point for the FetchKit Android SDK. */
object FetchKit {
    init {
        System.loadLibrary("fetchkit_jni")
    }

    /** Returns the version of the linked C++ core. */
    fun version(): String = nativeVersion()

    private external fun nativeVersion(): String
}
