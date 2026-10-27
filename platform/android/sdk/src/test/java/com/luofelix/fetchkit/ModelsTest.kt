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
        assertEquals(listOf(1L, 0L, 1L, 0L, 1L), request.numbers().toList())
        assertEquals(listOf("X-Test", "value"), request.texts().takeLast(2))
    }
}
