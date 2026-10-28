package com.luofelix.fetchkit

import org.junit.Assert.*
import org.junit.Test

class DiagnosticsTest {
    @Test fun levelsIncludeOnlyEnabledSeverities() {
        val expected = mapOf(
            LogLevel.Off to emptyList(),
            LogLevel.Error to listOf(LogLevel.Error),
            LogLevel.Warn to listOf(LogLevel.Error, LogLevel.Warn),
            LogLevel.Debug to listOf(LogLevel.Error, LogLevel.Warn, LogLevel.Debug),
        )
        expected.forEach { (configured, included) ->
            assertEquals(included, LogLevel.entries.filter(configured::includes))
        }
    }

    @Test fun diagnosticSelectsSafeContextWithoutReadingExceptionText() {
        val failure = object : RuntimeException() {
            override val message: String get() = throw AssertionError("禁止读取异常原文")
            override fun toString(): String = throw AssertionError("禁止输出异常对象")
        }
        val text = diagnosticMessage(DiagnosticEvent.ListenerFailed, failure, 7, 3)
        assertTrue(text.contains("taskId=7"))
        assertTrue(text.contains("generation=3"))
        val statusText = diagnosticMessage(DiagnosticEvent.ObserverCancelFailed, FetchKitException(Status.Busy))
        assertTrue(statusText.contains("status=Busy"))
    }

    @Test fun requestStringsHideCredentialsWithoutChangingRequestValues() {
        val url = "https://example.org/file?signature=url-private-secret"
        val info = RequestInfo(url, mapOf("Authorization" to "Bearer header-private-secret", "Cookie" to "cookie-private-secret"))
        val request = DownloadRequest(ResourceIdentity("account-private-secret", "key", "v1"), info,
            addressReference = "reference-private-secret")
        listOf(info.toString(), request.toString()).forEach { text ->
            assertFalse(text.contains("private-secret"))
            assertFalse(text.contains("https://"))
            assertFalse(text.contains("Authorization"))
            assertFalse(text.contains("Cookie"))
        }
        assertEquals(url, info.url)
        assertTrue(request.texts().contains("Bearer header-private-secret"))
    }
}
