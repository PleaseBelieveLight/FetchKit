package com.luofelix.fetchkit.sample

import android.os.Build
import android.os.Debug
import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.luofelix.fetchkit.*
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.net.Authenticator
import java.net.CookieHandler
import java.net.PasswordAuthentication
import java.net.URI
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/** Runs the production JNI/backend, never the M0 probe. Use tools/ci/run_m4_android.py. */
@RunWith(AndroidJUnit4::class)
class DownloadIntegrationTest {
    private fun <T> CompletableFuture<T>.await(): T = get(45, TimeUnit.SECONDS)
    private fun hash(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }
    private fun content(changed: Boolean = false, extra: Boolean = false): ByteArray =
        ByteArray(262144) { ((it + if (changed) 7 else 0) % 251).toByte() } + if (extra) "more".toByteArray() else byteArrayOf()
    private fun waitFor(sub: DownloadSubscription, predicate: (TaskSnapshot) -> Boolean): TaskSnapshot {
        val deadline = SystemClock.elapsedRealtime() + 10000
        while (SystemClock.elapsedRealtime() < deadline) {
            val value = sub.query().await()
            if (predicate(value)) return value
            if (value.state == TaskState.Failed || value.state == TaskState.Cancelled) {
                throw AssertionError("Early terminal ${value.state}: ${value.error}")
            }
            SystemClock.sleep(10)
        }
        error("Snapshot deadline")
    }
    private fun assertError(status: Status, block: () -> Any?) {
        try { block(); fail("Expected $status") }
        catch (error: Exception) {
            var cause: Throwable = error
            while (cause.cause != null) cause = requireNotNull(cause.cause)
            assertTrue("Expected $status, got ${cause.javaClass.simpleName}", cause is FetchKitException)
            assertEquals(status, (cause as FetchKitException).status)
        }
    }
    private fun readHash(engine: DownloadEngine, id: String): Pair<Long, String> {
        val lease = engine.acquire(id).await()
        try {
            var offset = 0L
            val digest = MessageDigest.getInstance("SHA-256")
            while (true) {
                val bytes = lease.read(offset).await()
                if (bytes.isEmpty()) break
                digest.update(bytes); offset += bytes.size
            }
            return offset to digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
        } finally { lease.releaseAsync().await() }
    }

    @Test fun productionDownloadMatrix() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val args = InstrumentationRegistry.getArguments()
        val base = requireNotNull(args.getString("fetchkitM4Url")) { "Run tools/ci/run_m4_android.py with a fixture" }
        val tls = requireNotNull(args.getString("fetchkitM4Tls"))
        val untrusted = requireNotNull(args.getString("fetchkitM4UntrustedTls"))
        val runId = requireNotNull(args.getString("fetchkitM4RunId"))
        val root = File(context.filesDir, "m4-tests-${UUID.randomUUID()}").canonicalFile
        assertEquals(context.filesDir.canonicalFile, root.parentFile)
        val cases = JSONArray()
        val report = JSONObject().put("run_id", runId).put("api", Build.VERSION.SDK_INT)
            .put("utc_ms", System.currentTimeMillis())
            .put("abi", Build.SUPPORTED_ABIS[0]).put("model", Build.MODEL).put("fingerprint", Build.FINGERPRINT)
            .put("backend", "OkHttp 5.5.1 production JNI").put("cases", cases)
        var failures = 0
        fun config(name: String, retries: Int = 0, max: Long = 64L * 1024 * 1024) =
            EngineConfig(File(root, name), max, 128L * 1024 * 1024, 86400000,
                allowHttp = true, maxRetries = retries, readTimeoutMs = 1500, retryBaseDelayMs = 50, retryMaxDelayMs = 100)
        fun request(name: String, url: String = "$base/m1/$name", expected: ByteArray? = content()) =
            DownloadRequest(ResourceIdentity("android", name, "v1"), RequestInfo(url),
                expectedSha256 = expected?.let(::hash) ?: "", requireDigest = expected != null)
        fun test(name: String, body: () -> Unit) {
            val item = JSONObject().put("id", name).put("passed", false)
            cases.put(item); val started = SystemClock.elapsedRealtime()
            try { body(); item.put("passed", true) }
            catch (error: Throwable) {
                failures++
                var cause = error
                while (cause.cause != null) cause = requireNotNull(cause.cause)
                item.put("error", if (cause is FetchKitException) cause.status.name else cause.javaClass.simpleName)
                if (cause is AssertionError) item.put("assertion", cause.message?.take(256))
            } finally { item.put("milliseconds", SystemClock.elapsedRealtime() - started) }
        }
        fun withEngine(name: String, retries: Int = 0, max: Long = 64L * 1024 * 1024, body: (DownloadEngine) -> Unit) {
            val engine = FetchKit.open(config(name, retries, max)).await()
            try { body(engine) } finally {
                try {
                    val snapshots = JSONArray()
                    engine.listTasks().await().forEach { item ->
                        snapshots.put(JSONObject().put("state", item.snapshot.state.name).put("error", item.snapshot.error.name)
                            .put("platform_code", item.snapshot.platformCode).put("requests", item.snapshot.requestCount)
                            .put("received_bytes", item.snapshot.receivedBytes).put("committed_offset", item.snapshot.committedOffset))
                    }
                    cases.getJSONObject(cases.length() - 1).put("snapshots", snapshots)
                } catch (_: Exception) {}
                engine.closeAsync().await()
            }
        }
        try {
            test("loopback-network-control") {
                // Independent test-only client distinguishes emulator packet loss from JNI/storage errors.
                repeat(10) { iteration ->
                    val connection = java.net.URL("$base/m1/network-control-$iteration").openConnection(java.net.Proxy.NO_PROXY) as java.net.HttpURLConnection
                    connection.setRequestProperty("Accept-Encoding", "identity")
                    connection.setRequestProperty("Connection", "close")
                    connection.connectTimeout = 5000; connection.readTimeout = 5000
                    try {
                        val actual = MessageDigest.getInstance("SHA-256")
                        var bytes = 0L
                        connection.inputStream.use { input ->
                            val block = ByteArray(65536)
                            while (true) {
                                val count = input.read(block)
                                if (count < 0) break
                                actual.update(block, 0, count); bytes += count
                                SystemClock.sleep(20)
                            }
                        }
                        assertEquals(262144L, bytes)
                        assertArrayEquals(MessageDigest.getInstance("SHA-256").digest(content()), actual.digest())
                    } finally { connection.disconnect() }
                }
            }
            for (mode in listOf("normal", "empty", "unknown", "unknown-close", "no-head", "same-origin")) test(mode) {
                val expected = if (mode == "empty") byteArrayOf() else content()
                withEngine(mode) { engine ->
                    val sub = engine.enqueue(request(mode, expected = expected)).await()
                    val done = sub.awaitStopped().await()
                    assertEquals(TaskState.Completed, done.state)
                    assertEquals(Integrity.DigestVerified, done.integrity)
                    assertEquals(expected.size.toLong() to hash(expected), readHash(engine, done.persistentId))
                }
            }
            val modes = listOf("resume-normal", "resume-ignore", "resume-partial", "resume-416", "resume-complete-416",
                "resume-changed", "resume-length-changed", "resume-bad-start", "resume-bad-end", "resume-bad-total",
                "resume-overflow", "resume-missing-range", "resume-missing-etag", "resume-weak206", "weak-etag", "no-etag", "last-modified", "resume-unknown",
                "resume-on-error", "resume-auth")
            for (mode in modes) test(mode) {
                val changed = mode in listOf("resume-ignore", "resume-changed", "resume-length-changed")
                val expected = content(changed, mode == "resume-length-changed")
                withEngine(mode) { engine ->
                    val r = request(mode, expected = expected).let { value ->
                        if (mode == "resume-auth") value.copy(provider = RequestProvider { c ->
                            RequestInfo(c.proposedUrl, if (c.reason == RequestReason.AuthRefresh) mapOf("Authorization" to "synthetic") else emptyMap())
                        }) else value
                    }
                    val sub = engine.enqueue(r).await()
                    if (mode == "resume-on-error") assertEquals(Status.Network, sub.awaitStopped().await().error)
                    else {
                        waitFor(sub) { it.receivedBytes >= 1024 }
                        assertEquals(TaskState.Paused, sub.pause().await().state)
                    }
                    sub.resume().await()
                    val done = sub.awaitStopped().await()
                    assertEquals(TaskState.Completed, done.state)
                    assertEquals(expected.size.toLong() to hash(expected), readHash(engine, done.persistentId))
                }
            }
            for ((mode, expectedError) in listOf("resume-short206" to Status.Network, "resume-extra206" to Status.ProtocolMismatch,
                "resume-invalid-after-restart" to Status.ProtocolMismatch)) test(mode) { withEngine(mode) { engine ->
                val sub = engine.enqueue(request(mode)).await(); waitFor(sub) { it.receivedBytes >= 1024 }
                sub.pause().await(); sub.resume().await()
                assertEquals(expectedError, sub.awaitStopped().await().error)
            } }
            for ((mode, status) in listOf("gzip" to Status.ProtocolMismatch, "conflicting-length" to Status.ProtocolMismatch,
                "overflow-length" to Status.ProtocolMismatch, "fresh-416" to Status.ProtocolMismatch,
                "not-found" to Status.HttpStatus, "truncated" to Status.Network, "timeout" to Status.Timeout,
                "redirect-loop" to Status.ProtocolMismatch, "redirect-chain" to Status.ProtocolMismatch,
                "userinfo-redirect" to Status.ProtocolMismatch)) test(mode) {
                withEngine(mode) { engine ->
                    val done = engine.enqueue(request(mode)).await().awaitStopped().await()
                    assertEquals(TaskState.Failed, done.state); assertEquals(status, done.error)
                    assertFalse(done.available)
                }
            }
            test("digest-mismatch") { withEngine("digest-mismatch") { engine ->
                val done = engine.enqueue(request("digest-mismatch", "$base/m1/normal", byteArrayOf(7))).await().awaitStopped().await()
                assertEquals(Status.IntegrityMismatch, done.error); assertFalse(done.available)
            } }
            test("length-mismatch") { withEngine("length-mismatch") { engine ->
                val done = engine.enqueue(request("length-mismatch").copy(expectedLength = 262145)).await().awaitStopped().await()
                assertEquals(Status.IntegrityMismatch, done.error); assertFalse(done.available)
            } }
            test("opaque-validator-byte-safety") { withEngine("opaque-etag") { engine ->
                val done = engine.enqueue(request("opaque-etag", "$base/m4/nonascii-etag")).await().awaitStopped().await()
                assertEquals(Status.ProtocolMismatch, done.error); assertFalse(done.available)
            } }
            test("utf8-request-header") { withEngine("request-unicode") { engine ->
                val r = request("request-unicode", "$base/m4/request-unicode").copy(
                    request = RequestInfo("$base/m4/request-unicode", mapOf("X-Text" to "中文")))
                val done = engine.enqueue(r).await().awaitStopped().await()
                assertEquals(TaskState.Completed, done.state); assertEquals(Integrity.DigestVerified, done.integrity)
            } }
            test("unknown-limit") { withEngine("unknown-limit", max = 1024) { engine ->
                val done = engine.enqueue(request("unknown-limit", expected = null)).await().awaitStopped().await()
                assertEquals(Status.QuotaExceeded, done.error); assertFalse(done.available)
            } }
            test("tls-normal") { withEngine("tls-normal") { engine ->
                assertEquals(TaskState.Completed, engine.enqueue(request("tls-normal", "$tls/m1/normal")).await().awaitStopped().await().state)
            } }
            test("untrusted-tls") { withEngine("untrusted-tls") { engine ->
                assertEquals(Status.CertificateError, engine.enqueue(request("untrusted-tls", "$untrusted/m1/normal")).await().awaitStopped().await().error)
            } }
            test("downgrade") { withEngine("downgrade") { engine ->
                assertEquals(Status.ProtocolMismatch, engine.enqueue(request("downgrade", "$tls/m1/downgrade")).await().awaitStopped().await().error)
            } }
            for (status in listOf(408, 429, 500, 502, 503, 504)) test("retry-$status") { withEngine("retry-$status", retries = 1) { engine ->
                val done = engine.enqueue(request("retry-$status", "$base/m3/retry-$status?case=android-$status&after=0")).await().awaitStopped().await()
                assertEquals(TaskState.Completed, done.state); assertEquals(1, done.retryCount); assertEquals(2, done.requestCount)
            } }
            test("no-hidden-503-retry") { withEngine("no-hidden-503-retry") { engine ->
                val done = engine.enqueue(request("no-hidden-503-retry", "$base/m3/retry-503?case=android-no-hidden&after=0")).await().awaitStopped().await()
                assertEquals(Status.HttpStatus, done.error); assertEquals(1, done.requestCount)
            } }
            for (after in listOf("1", "date")) test("retry-after-$after") { withEngine("retry-after-$after", retries = 1) { engine ->
                val started = SystemClock.elapsedRealtime()
                val done = engine.enqueue(request("retry-after-$after", "$base/m3/retry-429?case=android-after-$after&after=$after")).await().awaitStopped().await()
                assertEquals(TaskState.Completed, done.state); assertTrue(SystemClock.elapsedRealtime() - started >= 850)
            } }
            test("auth-refresh") { withEngine("auth-refresh") { engine ->
                val refreshes = AtomicInteger()
                val r = request("auth-refresh").copy(provider = RequestProvider { c ->
                    if (c.reason == RequestReason.AuthRefresh) refreshes.incrementAndGet()
                    RequestInfo(c.proposedUrl, if (c.reason == RequestReason.AuthRefresh) mapOf("Authorization" to "synthetic") else emptyMap())
                })
                assertEquals(TaskState.Completed, engine.enqueue(r).await().awaitStopped().await().state)
                assertEquals(1, refreshes.get())
            } }
            test("provider-throws") { withEngine("provider-throws") { engine ->
                assertEquals(Status.AuthRequired, engine.enqueue(request("provider-throws").copy(provider = RequestProvider { error("synthetic") }))
                    .await().awaitStopped().await().error)
            } }
            test("auth-refresh-budget") { withEngine("auth-budget") { engine ->
                val refreshes = AtomicInteger()
                val r = request("auth-budget", "$base/m1/auth-fail").copy(provider = RequestProvider { c ->
                    if (c.reason == RequestReason.AuthRefresh) refreshes.incrementAndGet()
                    RequestInfo(c.proposedUrl)
                })
                val done = engine.enqueue(r).await().awaitStopped().await()
                assertEquals(Status.AuthRequired, done.error); assertEquals(2, done.requestCount); assertEquals(1, refreshes.get())
            } }
            test("ambient-credentials-and-cross-origin") {
                val oldCookie = CookieHandler.getDefault(); val authCalls = AtomicInteger()
                CookieHandler.setDefault(object : CookieHandler() {
                    override fun get(uri: URI?, headers: MutableMap<String, MutableList<String>>?) = mutableMapOf("Cookie" to mutableListOf("synthetic"))
                    override fun put(uri: URI?, headers: MutableMap<String, MutableList<String>>?) {}
                })
                Authenticator.setDefault(object : Authenticator() { override fun getPasswordAuthentication(): PasswordAuthentication {
                    authCalls.incrementAndGet(); return PasswordAuthentication("synthetic", "synthetic".toCharArray())
                } })
                try { withEngine("ambient") { engine ->
                    val plain = engine.enqueue(request("ambient-normal", "$base/m1/ambient-normal")).await().awaitStopped().await()
                    assertEquals(TaskState.Completed, plain.state)
                    val auth = engine.enqueue(request("ambient-auth", "$base/m1/auth-fail")).await().awaitStopped().await()
                    assertEquals(Status.AuthRequired, auth.error); assertEquals(0, authCalls.get())
                    val cross = request("cross-origin").copy(request = RequestInfo("$base/m1/cross-origin",
                        mapOf("Authorization" to "synthetic", "Cookie" to "synthetic", "X-Api-Key" to "synthetic")))
                    assertEquals(TaskState.Completed, engine.enqueue(cross).await().awaitStopped().await().state)
                } } finally {
                    CookieHandler.setDefault(oldCookie)
                    // Android has no portable Authenticator getter; this is a fresh test process.
                    Authenticator.setDefault(null)
                }
            }
            test("shared-cancel-recovery-lease-close") {
                val c = config("shared")
                var engine = FetchKit.open(c).await()
                val r = request("shared", "$base/m1/resume-shared", content()).copy(identity = ResourceIdentity("中文", "emoji-😀", "v1"))
                var lease: ResourceLease? = null
                try {
                    val first = engine.enqueue(r).await(); val other = engine.enqueue(r).await()
                    assertNotEquals(first.taskId, other.taskId); waitFor(other) { it.receivedBytes >= 1024 }
                    assertEquals(TaskState.Cancelled, first.cancel().await().state)
                    val paused = other.pause().await(); assertTrue(paused.committedOffset > 0)
                    engine.closeAsync().await(); engine = FetchKit.open(c).await()
                    val recovered = engine.listTasks().await().single()
                    assertEquals(r.identity, recovered.identity); assertEquals(paused.committedOffset, recovered.snapshot.committedOffset)
                    val rebound = engine.enqueue(r).await(); val done = rebound.awaitStopped().await()
                    assertEquals(TaskState.Completed, done.state)
                    lease = engine.acquire(done.persistentId).await()
                    engine.closeAsync().await()
                    assertEquals(64, lease.read(0, 64).await().size)
                    assertError(Status.Busy) { FetchKit.open(c).await() }
                    lease.releaseAsync().await(); lease = null
                    engine = FetchKit.open(c).await()
                    assertEquals(262144L to hash(content()), readHash(engine, done.persistentId))
                    assertError(Status.Closed) { first.query().await() }
                } finally { lease?.releaseAsync()?.await(); engine.closeAsync().await() }
            }
            test("slow-executor-close") { withEngine("slow-executor") { engine ->
                val pending = java.util.concurrent.LinkedBlockingQueue<Runnable>(1); val callbacks = AtomicInteger()
                val sub = engine.enqueue(request("slow-executor", "$base/m1/resume-slow-executor")).await()
                val observer = sub.observe(Executor { assertTrue(pending.offer(it)) }) { callbacks.incrementAndGet() }
                assertNotNull(pending.poll(5, TimeUnit.SECONDS)?.also { pending.offer(it) })
                val waiter = sub.awaitStopped(); engine.closeAsync().await()
                pending.poll()?.run(); assertEquals(0, callbacks.get())
                try { assertEquals(TaskState.Paused, waiter.await().state) }
                catch (error: Exception) { assertError(Status.Closed) { throw error } }
                observer.close()
            } }
            test("future-close-reentrancy") { withEngine("future-close") { engine ->
                val sub = engine.enqueue(request("future-close", "$base/m1/resume-future-close")).await()
                waitFor(sub) { it.receivedBytes >= 1024 }
                val callback = java.util.concurrent.CountDownLatch(1)
                sub.awaitStopped().whenComplete { _, _ -> engine.closeAsync().await(); callback.countDown() }
                engine.closeAsync().await()
                assertTrue(callback.await(5, TimeUnit.SECONDS))
            } }
            test("header-absolute-deadline") {
                val engine = FetchKit.open(config("header-deadline").copy(readTimeoutMs = 300)).await()
                try {
                    val started = SystemClock.elapsedRealtime()
                    val done = engine.enqueue(request("header-deadline", "$base/m4/slow-headers", null)).await().awaitStopped().await()
                    assertEquals(Status.Timeout, done.error)
                    assertTrue(SystemClock.elapsedRealtime() - started < 2000)
                } finally { engine.closeAsync().await() }
            }
            test("pin-expiry-remove-lease-bindings") { withEngine("cache-policy") { engine ->
                val sub = engine.enqueue(request("cache", "$base/m1/normal").copy(pinOnCreate = true)).await()
                val done = sub.awaitStopped().await(); assertEquals(TaskState.Completed, done.state)
                val lease = engine.acquire(done.persistentId).await()
                try {
                    assertError(Status.Busy) { engine.remove(done.persistentId).await() }
                    engine.pin(done.persistentId, false).await()
                    assertError(Status.QuotaExceeded) { engine.trimTo(0).await() }
                    engine.setExpiry(done.persistentId, System.currentTimeMillis() - 1).await()
                    assertError(Status.NotFound) { engine.acquire(done.persistentId).await() }
                    assertEquals(64, lease.read(0, 64).await().size)
                } finally { lease.releaseAsync().await() }
                engine.remove(done.persistentId).await(); engine.collectStaging().await(); engine.trimTo(0).await()
                assertEquals(0L, engine.storageStats().await().chargedBytes)
            } }
            for (missing in listOf("object", "record")) test("partial-restore-$missing") {
                val c = config("partial-$missing")
                var engine = FetchKit.open(c).await()
                try {
                    val done = engine.enqueue(request("partial-$missing", "$base/m1/normal")).await().awaitStopped().await()
                    assertEquals(TaskState.Completed, done.state); engine.closeAsync().await()
                    val directory = File(c.storageRoot, if (missing == "object") "objects" else "records").canonicalFile
                    val file = File(directory, done.persistentId + if (missing == "object") ".obj" else ".rec").canonicalFile
                    assertEquals(directory, file.parentFile); assertTrue(file.delete())
                    engine = FetchKit.open(c).await()
                    assertError(Status.NotFound) { engine.acquire(done.persistentId).await() }
                    if (missing == "object") assertFalse(engine.listTasks().await().single().snapshot.available)
                    else {
                        assertTrue(engine.listTasks().await().isEmpty())
                        assertTrue(engine.storageStats().await().orphanFiles > 0)
                        assertError(Status.QuotaExceeded) { engine.trimTo(0).await() }
                    }
                } finally { engine.closeAsync().await() }
            }
            test("retry-wait-across-reopen") {
                val c = config("retry-reopen", retries = 1)
                var engine = FetchKit.open(c).await()
                val r = request("retry-reopen", "$base/m3/retry-503?case=android-wait-reopen&after=120")
                try {
                    val sub = engine.enqueue(r).await(); val waiting = waitFor(sub) { it.state == TaskState.RetryWaiting }
                    engine.closeAsync().await(); engine = FetchKit.open(c).await()
                    assertEquals(waiting.nextRetryAtMs, engine.listTasks().await().single().snapshot.nextRetryAtMs)
                    val attached = engine.attach(r).await(); attached.resume().await(); SystemClock.sleep(150)
                    assertEquals(waiting.nextRetryAtMs, attached.query().await().nextRetryAtMs)
                    assertEquals(TaskState.Cancelled, attached.cancel().await().state)
                } finally { engine.closeAsync().await() }
            }
            test("provider-cancel-and-expired-scope") { withEngine("provider-cancel") { engine ->
                val entered = java.util.concurrent.CountDownLatch(1)
                val saved = java.util.concurrent.atomic.AtomicReference<RequestContext>()
                val r = request("provider-cancel").copy(provider = RequestProvider { c ->
                    saved.set(c); entered.countDown()
                    while (!c.isCancelled()) SystemClock.sleep(5)
                    throw IllegalStateException("Cancelled")
                })
                val sub = engine.enqueue(r).await(); assertTrue(entered.await(5, TimeUnit.SECONDS))
                sub.releaseAsync().await(); assertTrue(saved.get().isCancelled())
                assertError(Status.Closed) { sub.query().await() }
            } }
            test("cancellation-release-stress") { withEngine("cancellation-stress") { engine ->
                val timings = JSONArray()
                repeat(20) { iteration ->
                    val sub = engine.enqueue(request("cancel-$iteration", "$base/m4/cancel-$iteration-$runId")).await()
                    waitFor(sub) { it.receivedBytes >= 1024 }
                    val started = SystemClock.elapsedRealtime()
                    assertEquals(TaskState.Cancelled, sub.cancel().await().state); sub.releaseAsync().await()
                    timings.put(SystemClock.elapsedRealtime() - started)
                    assertError(Status.Closed) { sub.query().await() }
                }
                report.put("cancel_to_release_ms_including_checkpoint", timings)
            } }
            test("parallel-stream-memory") { withEngine("stream-memory") { engine ->
                fun batch(label: String, size: Int): JSONObject {
                    val block = ByteArray(65536) { (it % 251).toByte() }
                    val digest = MessageDigest.getInstance("SHA-256")
                    repeat(size / block.size) { digest.update(block) }
                    val expected = digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
                    val baseline = Debug.getPss().toLong()
                    val peak = java.util.concurrent.atomic.AtomicLong(baseline)
                    val stopped = java.util.concurrent.atomic.AtomicBoolean()
                    val sampler = Thread {
                        while (!stopped.get()) {
                            peak.accumulateAndGet(Debug.getPss().toLong()) { old, value -> maxOf(old, value) }
                            SystemClock.sleep(50)
                        }
                    }
                    sampler.start()
                    try {
                        val subs = (0 until 3).map { index -> engine.enqueue(DownloadRequest(
                            ResourceIdentity("memory", "$label-$index", "v1"),
                            RequestInfo("$base/m3/stream?bytes=$size&slow=1"), expectedLength = size.toLong(),
                            expectedSha256 = expected, requireDigest = true)).await() }
                        subs.forEach { assertEquals(TaskState.Completed, it.awaitStopped().await().state) }
                    } finally { stopped.set(true); sampler.join(2000) }
                    return JSONObject().put("bytes_per_file", size).put("parallel_files", 3)
                        .put("baseline_pss_kib", baseline).put("peak_pss_kib", peak.get())
                }
                val small = batch("small", 262144)
                val large = batch("large", 32 * 1024 * 1024)
                val growth = large.getLong("peak_pss_kib") - small.getLong("peak_pss_kib")
                report.put("memory", JSONObject().put("small", small).put("large", large).put("peak_growth_kib", growth))
                assertTrue("PSS growth follows file size", growth < 32768)
                engine.trimTo(0).await()
            } }
            test("close-query-reopen-race") {
                val c = config("close-query-race")
                repeat(10) { round ->
                    val engine = FetchKit.open(c).await()
                    val sub = engine.enqueue(request("query-race-$round", "$base/m4/query-race-$round-$runId")).await()
                    waitFor(sub) { it.receivedBytes >= 1024 }
                    val queries = (0 until 10).map { sub.query() }
                    sub.awaitStopped(); sub.awaitStopped()
                    engine.closeAsync().await()
                    queries.forEach { query -> try { query.await() } catch (error: Exception) { assertError(Status.Closed) { throw error } } }
                    FetchKit.open(c).await().closeAsync().await()
                }
            }
            test("jobscheduler-system-stop") {
                var activity: android.app.Activity? = null
                fun shell(command: String): String = instrumentation.uiAutomation.executeShellCommand(command).use { descriptor ->
                    android.os.ParcelFileDescriptor.AutoCloseInputStream(descriptor).bufferedReader().use { it.readText() }
                }
                try {
                    if (Build.VERSION.SDK_INT >= 34) activity = instrumentation.startActivitySync(
                        android.content.Intent(context, MainActivity::class.java).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
                    val version = "job-$runId"
                    val jobUrl = "$base/m4/job-$runId"
                    assertTrue(DownloadJobService.schedule(context, jobUrl, hash(content()), version, false, false))
                    shell("cmd jobscheduler run -f ${context.packageName} ${DownloadJobService.JOB_ID}")
                    val deadline = SystemClock.elapsedRealtime() + 15000
                    var running = false
                    while (!running && SystemClock.elapsedRealtime() < deadline) {
                        instrumentation.runOnMainSync { running = SampleDownloads.jobRunning }
                        if (!running) SystemClock.sleep(20)
                    }
                    assertTrue("System did not start the job", running)
                    val engine = SampleDownloads.engine(context).await()
                    var stored = engine.listTasks().await().firstOrNull { it.identity.version == version }
                    while ((stored == null || stored.snapshot.receivedBytes < 1024) && SystemClock.elapsedRealtime() < deadline) {
                        SystemClock.sleep(10); stored = engine.listTasks().await().firstOrNull { it.identity.version == version }
                    }
                    assertNotNull(stored); assertTrue(requireNotNull(stored).snapshot.receivedBytes >= 1024)
                    val timeoutSupported = shell("cmd jobscheduler help").lineSequence().any {
                        it.trimStart().startsWith("timeout ")
                    }
                    val started = SystemClock.elapsedRealtime()
                    // API 24 的 shell 仅提供 run；通过真实 JobScheduler 取消触发系统 onStopJob，
                    // 不直接调用回调，也不跳过核心任务暂停与检查点恢复断言。
                    if (timeoutSupported) shell("cmd jobscheduler timeout ${context.packageName} ${DownloadJobService.JOB_ID}")
                    else context.getSystemService(android.app.job.JobScheduler::class.java).cancel(DownloadJobService.JOB_ID)
                    do {
                        instrumentation.runOnMainSync { running = SampleDownloads.jobRunning }
                        if (running) SystemClock.sleep(20)
                    } while (running && SystemClock.elapsedRealtime() - started < 10000)
                    assertFalse("System stop did not reach onStopJob", running)
                    context.getSystemService(android.app.job.JobScheduler::class.java).cancel(DownloadJobService.JOB_ID)
                    if (activity != null) {
                        val current = activity
                        instrumentation.runOnMainSync { current?.finish() }
                        instrumentation.waitForIdleSync()
                    }
                    SampleDownloads.stop().await()
                    val reopened = SampleDownloads.engine(context).await()
                    try {
                        val row = reopened.listTasks().await().single { it.identity.version == version }
                        assertEquals(TaskState.Paused, row.snapshot.state); assertTrue(row.snapshot.committedOffset >= 1024)
                        report.put("job_stop", JSONObject().put("api", Build.VERSION.SDK_INT)
                            .put("trigger", if (timeoutSupported) "shell-timeout" else "scheduler-cancel")
                            .put("uidt", Build.VERSION.SDK_INT >= 34).put("checkpoint_bytes", row.snapshot.committedOffset)
                            .put("milliseconds", SystemClock.elapsedRealtime() - started))
                        reopened.remove(row.snapshot.persistentId).await()
                    } finally { SampleDownloads.stop().await() }
                } finally {
                    context.getSystemService(android.app.job.JobScheduler::class.java).cancel(DownloadJobService.JOB_ID)
                    activity?.let { current -> instrumentation.runOnMainSync { current.finish() } }
                    SampleDownloads.stop().await()
                }
            }
            report.put("passed", failures == 0).put("failure_count", failures)
        } finally {
            report.put("passed", failures == 0).put("failure_count", failures)
            File(context.filesDir, "m4-android.json").writeText(report.toString(2))
            assertEquals(context.filesDir.canonicalFile, root.canonicalFile.parentFile)
            if (failures == 0) root.deleteRecursively()
        }
        assertEquals("M4 production JNI failures; see files/m4-android.json", 0, failures)
    }
}
