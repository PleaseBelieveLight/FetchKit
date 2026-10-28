package com.luofelix.fetchkit.sample

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.luofelix.fetchkit.EngineConfig
import com.luofelix.fetchkit.FetchKit
import com.luofelix.fetchkit.Integrity
import com.luofelix.fetchkit.Status
import com.luofelix.fetchkit.TaskState
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.net.InetAddress
import java.net.ServerSocket
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

@RunWith(AndroidJUnit4::class)
class SampleDownloadTest {
    private fun <T> CompletableFuture<T>.await(): T = get(15, TimeUnit.SECONDS)

    @Test fun httpsQueryUrlCanBeStartedBySample() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        try {
            val sub = SampleDownloads.bind(context, SampleDownloads.request(
                "https://speed.cloudflare.com/__down?bytes=1048576", "", "v1", false), true).await()
            assertTrue(sub.taskId > 0)
            sub.cancel().await()
        } finally { SampleDownloads.stop().await() }
    }

    @Test fun queryUrlWithoutDigestDownloadsAndCanBeRead() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(context.filesDir, "sample-test-${UUID.randomUUID()}")
        val body = ByteArray(1048576) { (it % 251).toByte() }
        val executor = Executors.newSingleThreadExecutor()
        try {
            ServerSocket(0, 1, InetAddress.getByName("127.0.0.1")).use { server ->
                server.soTimeout = 15000
                val response = executor.submit<String> {
                    server.accept().use { socket ->
                        socket.soTimeout = 15000
                        val reader = socket.getInputStream().bufferedReader(Charsets.US_ASCII)
                        val requestLine = reader.readLine()
                        val headers = generateSequence { reader.readLine() }.takeWhile { it.isNotEmpty() }.toList()
                        assertTrue("Sample application identity must reach the HTTP server",
                            headers.any { it.equals("User-Agent: $sampleUserAgent", ignoreCase = true) })
                        socket.getOutputStream().apply {
                            write("HTTP/1.1 200 OK\r\nContent-Length: ${body.size}\r\nConnection: close\r\n\r\n"
                                .toByteArray(Charsets.US_ASCII))
                            write(body)
                            flush()
                        }
                        requestLine
                    }
                }
                val engine = FetchKit.open(EngineConfig(root, 2L shl 30, 4L shl 30, 7L * 86400000,
                    allowHttp = true)).await()
                try {
                    val request = SampleDownloads.request(
                        "http://127.0.0.1:${server.localPort}/__down?bytes=${body.size}", "", "v1", false)
                    val sub = engine.enqueue(request).await()
                    val done = sub.awaitStopped().await()
                    assertEquals(Status.Ok, done.error)
                    assertEquals(TaskState.Completed, done.state)
                    assertEquals(body.size.toLong(), done.receivedBytes)
                    assertEquals(Integrity.TransportComplete, done.integrity)
                    val stored = engine.listTasks().await().single()
                    assertEquals(request.identity, stored.identity)
                    val lease = engine.acquire(stored.snapshot.persistentId).await()
                    try { assertArrayEquals(body.copyOf(64), lease.read(0, 64).await()) }
                    finally { lease.releaseAsync().await() }
                    sub.releaseAsync().await()
                    assertEquals("GET /__down?bytes=${body.size} HTTP/1.1", response.get(15, TimeUnit.SECONDS))
                } finally { engine.closeAsync().await() }
            }
        } finally {
            executor.shutdownNow()
            assertEquals(context.filesDir.canonicalFile, root.canonicalFile.parentFile)
            root.deleteRecursively()
        }
    }
}
