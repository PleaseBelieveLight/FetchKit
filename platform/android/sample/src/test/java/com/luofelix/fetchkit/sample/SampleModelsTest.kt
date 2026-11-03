package com.luofelix.fetchkit.sample

import org.junit.Assert.*
import org.junit.Test
import java.net.URI

class SampleModelsTest {
    @Test fun backgroundJobSlotsDoNotReplacePendingOrRunningJobs() {
        assertEquals(4100, selectDownloadJobId(false, setOf(4101)))
        assertNull(selectDownloadJobId(false, setOf(4100)))
        assertEquals(4103, selectDownloadJobId(true, setOf(4100, 4101, 4102, 4104)))
        assertNull(selectDownloadJobId(true, (4101..4115).toSet()))
    }

    @Test fun presetsAreDistinctAndKeepQueryParameters() {
        val requests = downloadPresets.map { it.request() }
        assertEquals(downloadPresets.size, requests.map { it.identity }.toSet().size)
        val ubuntu = requests.filter { "/ubuntu-releases/" in it.request.url }
        assertEquals(3, ubuntu.size)
        ubuntu.forEach { assertEquals("utm_source=chatgpt.com", URI(it.request.url).rawQuery) }
        assertEquals("270 MiB", downloadPresets.first().size)
        assertEquals("105 B", recoveryDemoFile.size)
        requests.forEach { assertEquals(sampleUserAgent, it.request.headers["User-Agent"]) }
        assertTrue(requests.all { it.expectedLength == null && !it.requireDigest })
        assertEquals(requests.first().identity, downloadPresets.first().request(pinned = true).identity)
        assertNotEquals(requests.first().identity, downloadPresets.first().request(version = "v2").identity)
    }

    @Test fun refreshAddressKeepsResourceIdentityAndUsesExplicitStatuses() {
        val request = sampleRequest("https://example.org/expired", "a".repeat(64), "v2", false)
        val updated = withSampleRefresh(request, "https://example.org/fresh", true)
        assertEquals(request.identity, updated.identity)
        assertEquals(request.expectedSha256, updated.expectedSha256)
        assertEquals(setOf(404, 410), updated.refreshHttpStatuses)
        assertNotNull(updated.provider)
    }

    @Test fun validatesOptionalDigestAndPreservesSignedAddressInMemory() {
        val digest = "AB".repeat(32)
        val request = sampleRequest(" https://example.org/file?signature=secret ", digest, " v2 ", true)
        assertEquals(digest.lowercase(), request.expectedSha256)
        assertTrue(request.requireDigest)
        assertEquals("v2", request.identity.version)
        assertEquals("sample-session", request.addressReference)
        assertTrue(request.pinOnCreate)
        assertEquals("https://example.org/file?signature=secret", request.request.url)
        assertEquals(sampleUserAgent, request.request.headers["User-Agent"])
        listOf("abc", "g".repeat(64)).forEach { invalid ->
            assertThrows(IllegalArgumentException::class.java) { sampleRequest("https://example.org/file", invalid, "v1", false) }
        }
        listOf("file:///tmp/file", "https://name:secret@example.org/file", "https://example.org/file#token").forEach { invalid ->
            assertThrows(IllegalArgumentException::class.java) { sampleRequest(invalid, "", "v1", false) }
        }
    }

    @Test fun sizesUseBinaryUnitsAndLongOffsets() {
        assertEquals("0 B", formatBytes(0))
        assertEquals("1.0 KiB", formatBytes(1024))
        assertEquals("500 MiB", formatBytes(524288000))
        assertEquals("4.0 GiB", formatBytes((1L shl 32) + 1))
    }
}
