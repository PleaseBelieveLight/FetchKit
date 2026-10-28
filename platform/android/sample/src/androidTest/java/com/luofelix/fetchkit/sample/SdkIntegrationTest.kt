package com.luofelix.fetchkit.sample

import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.luofelix.fetchkit.FetchKit
import com.luofelix.fetchkit.LogLevel

import org.junit.Test
import org.junit.runner.RunWith

import org.junit.Assert.*

/** 验证宿主标识与真实 JNI 接入，需连接 Android 设备。 */
@RunWith(AndroidJUnit4::class)
class SdkIntegrationTest {
    @Test
    fun useAppContext() {
        // 使用目标 App 的上下文，避免误校验测试 APK 的包名。
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        assertEquals("com.luofelix.fetchkit", appContext.packageName)
    }

    @Test
    fun sdkLoadsNativeCore() {
        assertTrue(FetchKit.version().matches(Regex("\\d+\\.\\d+\\.\\d+")))
    }

    @Test
    fun loggingLevelsCanChangeWithoutOpeningEngine() {
        try {
            LogLevel.entries.forEach(FetchKit::setLogLevel)
        } finally {
            FetchKit.setLogLevel(LogLevel.Off)
        }
    }
}
