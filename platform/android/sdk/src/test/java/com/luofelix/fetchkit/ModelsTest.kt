package com.luofelix.fetchkit

import org.junit.Assert.*
import org.junit.Test
import java.io.File

class ModelsTest {
    @Test fun configurationPreserves64BitLimits() {
        val config = EngineConfig(File("root"), 5L shl 30, 8L shl 30, 86400000)
        assertEquals(5L shl 30, config.numbers()[0])
        assertEquals(8L shl 30, config.numbers()[1])
        assertEquals(3, config.maxActiveDownloads)
    }
    @Test(expected = IllegalArgumentException::class) fun rejectsZeroCapacity() {
        EngineConfig(File("root"), 1024, 0, 1000)
    }
    @Test fun snapshotPreservesUnknownLengthAndIntegrity() {
        val row = listOf("7", "2", "9", "0", "4294967300", "4294967300", "0", "0", "2", "206", "-1",
            "0", "2", "1", "3", "0", "0", "1", "1", "a".repeat(64))
        val snapshot = TaskSnapshot.decode(row)
        assertEquals(TaskState.Completed, snapshot.state)
        assertNull(snapshot.totalBytes)
        assertEquals(4294967300L, snapshot.receivedBytes)
        assertEquals(Integrity.DigestVerified, snapshot.integrity)
        assertEquals(-1, snapshot.platformCode)
        assertFalse(snapshot.canRetryWithRequest)
        assertTrue(snapshot.copy(state = TaskState.Failed, error = Status.AuthRequired).canRetryWithRequest)
        assertFalse(snapshot.copy(state = TaskState.Failed, error = Status.IntegrityMismatch).canRetryWithRequest)
    }
    @Test fun eventMapsSubscriptionOverflow() {
        val event = TaskEvent.decode(listOf("2", "19", "4", "4", "21", "3", "2", "7", "1", "0", "503", "2", "12345"))
        assertEquals(Status.SubscriptionOverflow, event.error)
        assertEquals(EventKind.SubscriptionError, event.kind)
        assertEquals(12345L, event.nextRetryAtMs)
    }
    @Test fun requestProviderDoesNotEnterPersistentIdentity() {
        val identity = ResourceIdentity("模型", "emoji-😀", "v2")
        val request = DownloadRequest(identity, RequestInfo("https://example.com/file", mapOf("X-Test" to "value")),
            expectedLength = 0, requireDigest = true, expectedSha256 = "a".repeat(64), pinOnCreate = true)
        assertEquals("emoji-😀", request.texts()[1])
        assertEquals(listOf(1L, 0L, 1L, 0L, 1L, 0L), request.numbers().toList())
        assertEquals(listOf("X-Test", "value"), request.texts().takeLast(2))
    }
    @Test fun refreshPolicyAndProviderFailuresAreBounded() {
        val r = DownloadRequest(ResourceIdentity("test", "key", "v1"), refreshHttpStatuses = setOf(404, 410))
        assertEquals(3L, r.numbers().last())
        assertEquals(Status.Timeout, RequestProviderException(Status.Timeout).status)
    }
    @Test(expected = IllegalArgumentException::class) fun rejectsArbitraryRefreshStatus() {
        DownloadRequest(ResourceIdentity("test", "key", "v1"), refreshHttpStatuses = setOf(500))
    }
    @Test(expected = IllegalArgumentException::class) fun rejectsArbitraryProviderStatus() {
        RequestProviderException(Status.DiskFull)
    }
    @Test fun typedProviderBridgeUsesOnlySafeStatus() {
        val provider = NativeProvider(RequestProvider { throw RequestProviderException(Status.Network) })
        assertEquals(listOf(Status.Network.ordinal.toString()), provider.provide(0, 0, 1, 1, 0, "", "", "").toList())
    }
    @Test fun providerBridgePreservesSuccessfulRequest() {
        val provider = NativeProvider(RequestProvider { RequestInfo("https://example.org/file", mapOf("X-Test" to "synthetic")) })
        assertEquals(listOf("0", "https://example.org/file", "X-Test", "synthetic"),
            provider.provide(0, 0, 1, 1, 0, "", "", "").toList())
    }
}
