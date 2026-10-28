package com.luofelix.fetchkit.sample

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.luofelix.fetchkit.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.net.InetAddress
import java.net.ServerSocket
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** Local HTTP, real SDK and the same row controls used by Compose; no public large-file traffic. */
@RunWith(AndroidJUnit4::class)
class SampleControlsTest {
    private fun <T> CompletableFuture<T>.await(): T = get(25, TimeUnit.SECONDS)
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val context get() = instrumentation.targetContext

    private fun waitUntil(predicate: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(20)
        while (!predicate()) {
            check(System.nanoTime() < deadline) { "Condition did not complete" }
            Thread.sleep(20)
        }
    }
    private fun action(block: () -> Unit) {
        instrumentation.runOnMainSync(block)
        waitUntil { !SampleDownloads.busy }
    }

    @Test fun concurrentRowsResumeTheirOwnAddressAndKeepOtherSubscriptionsAlive() {
        val pool = Executors.newFixedThreadPool(4)
        val rangeOffsets = CopyOnWriteArrayList<Long>()
        val requests = mutableListOf<DownloadRequest>()
        val body = ByteArray(4 * 1024 * 1024) { (it % 251).toByte() }
        val hash = MessageDigest.getInstance("SHA-256").digest(body).joinToString("") { "%02x".format(it.toInt() and 255) }
        val server = ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"))
        server.soTimeout = 25000
        try {
            pool.submit {
                while (!server.isClosed) {
                    val socket = try { server.accept() } catch (_: Exception) { break }
                    pool.submit {
                        socket.use {
                            runCatching {
                                socket.soTimeout = 20000
                                val reader = socket.getInputStream().bufferedReader(Charsets.US_ASCII)
                                val requestLine = reader.readLine()
                                var offset = 0L
                                while (true) {
                                    val line = reader.readLine() ?: break
                                    if (line.isEmpty()) break
                                    if (line.startsWith("Range:", ignoreCase = true)) offset = line.substringAfter("bytes=").substringBefore('-').toLong()
                                }
                                if (requestLine.contains("/first") && offset > 0) rangeOffsets.add(offset)
                                val headers = if (offset == 0L) "HTTP/1.1 200 OK\r\n" else
                                    "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes $offset-${body.size - 1}/${body.size}\r\n"
                                val output = socket.getOutputStream()
                                output.write((headers + "Content-Length: ${body.size - offset}\r\nETag: \"stable\"\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n").toByteArray(Charsets.US_ASCII))
                                var position = offset.toInt()
                                while (position < body.size) {
                                    val count = minOf(16384, body.size - position)
                                    output.write(body, position, count); output.flush(); position += count
                                    Thread.sleep(10)
                                }
                            }
                        }
                    }
                }
            }
            SampleDownloads.stop().await()
            val version = "sample-test-${UUID.randomUUID()}"
            requests += sampleRequest("http://127.0.0.1:${server.localPort}/first", hash, version, false)
            requests += sampleRequest("http://127.0.0.1:${server.localPort}/second", hash, version, false)
            val first = SampleDownloads.bind(context, requests[0], true).await()
            val second = SampleDownloads.bind(context, requests[1], true).await()
            waitUntil { first.query().await().receivedBytes >= 131072 && second.query().await().receivedBytes >= 131072 }
            SampleDownloads.refresh(context).await()
            val row = SampleDownloads.rows.single { it.identity == requests[0].identity }
            assertEquals(requests[0].request.url, SampleDownloads.requestFor(row)!!.request.url)
            action { SampleDownloads.control(context, row, "pause") }
            val paused = first.query().await()
            assertEquals(TaskState.Paused, paused.state)
            assertTrue(paused.committedOffset > 0)
            assertNotEquals(TaskState.Paused, second.query().await().state)
            action { SampleDownloads.control(context, row, "subscribe") }
            assertEquals(2, first.query().await().subscriberCount)
            action { SampleDownloads.control(context, row, "cancel") }
            assertEquals(1, first.query().await().subscriberCount)
            action { SampleDownloads.control(context, row, "resume") }
            assertEquals(TaskState.Completed, first.awaitStopped().await().state)
            assertEquals(TaskState.Completed, second.awaitStopped().await().state)
            assertTrue("Resume must use Range on the first URL", rangeOffsets.contains(paused.committedOffset))
            assertEquals(Integrity.DigestVerified, first.query().await().integrity)
            SampleDownloads.refresh(context).await()
            val complete = SampleDownloads.rows.single { it.identity == requests[0].identity }
            action { SampleDownloads.read(context, complete) }
            assertEquals(complete.snapshot.persistentId, SampleDownloads.leasedId)
            val engine = SampleDownloads.engine(context).await()
            val protected = runCatching { engine.remove(complete.snapshot.persistentId).await() }.exceptionOrNull()
            assertNotNull("Lease must protect resource removal", protected)
            action { SampleDownloads.releaseRead(context) }
            assertNull(SampleDownloads.leasedId)
            action { SampleDownloads.pin(context, complete) }
            val pinned = engine.listTasks().await().single { it.identity == complete.identity }.snapshot
            assertTrue("listTasks must reflect a policy update while the task is still live", pinned.pinned)
            assertEquals(first.query().await().pinned, pinned.pinned)
            engine.pin(complete.snapshot.persistentId, false).await()
            val expiredAt = System.currentTimeMillis() - 1
            action { SampleDownloads.expiry(context, complete, expiredAt) }
            val expired = engine.listTasks().await().single { it.identity == complete.identity }.snapshot
            assertEquals(expiredAt, expired.expiresAtMs)
            assertFalse(expired.available)
        } finally {
            SampleDownloads.stop().await()
            server.close()
            pool.shutdownNow()
            val engine = SampleDownloads.engine(context).await()
            engine.listTasks().await().filter { task -> requests.any { it.identity == task.identity } }.forEach { task ->
                engine.pin(task.snapshot.persistentId, false).await()
                engine.remove(task.snapshot.persistentId).await()
            }
            SampleDownloads.stop().await()
        }
    }
}
