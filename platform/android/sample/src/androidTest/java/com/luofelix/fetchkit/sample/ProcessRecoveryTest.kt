package com.luofelix.fetchkit.sample

import android.os.Process
import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.luofelix.fetchkit.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.CompletableFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/** Controller kills a live writer, then starts a new instrumentation process with fresh credentials. */
@RunWith(AndroidJUnit4::class)
class ProcessRecoveryTest {
    private fun <T> CompletableFuture<T>.await(timeout: Long = 45) = get(timeout, TimeUnit.SECONDS)
    @Test fun checkpointAcrossProcessDeath() {
        val args = InstrumentationRegistry.getArguments()
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val kind = requireNotNull(args.getString("fetchkitM4DeathKind"))
        require(kind in setOf("kill", "force-stop", "large"))
        val large = kind == "large"
        val phase = requireNotNull(args.getString("fetchkitM4Phase"))
        require(phase in setOf("prepare", "restore"))
        val runId = requireNotNull(args.getString("fetchkitM4RunId"))
        val base = requireNotNull(args.getString("fetchkitM4Url"))
        val root = File(context.filesDir, "m4-process-$kind").canonicalFile
        assertEquals(context.filesDir.canonicalFile, root.parentFile)
        if (phase == "prepare" && root.exists()) assertTrue(root.deleteRecursively())
        if (large && phase == "prepare") assertTrue("Actual >4 GiB test needs at least 4.8 GiB free",
            android.os.StatFs(context.filesDir.absolutePath).availableBytes > 4800L * 1024 * 1024)
        val config = EngineConfig(root, if (large) 8L shl 30 else 64L * 1024 * 1024,
            if (large) 12L shl 30 else 128L * 1024 * 1024, 86400000,
            allowHttp = true, maxRetries = 0, readTimeoutMs = 300000, checkpointBytes = if (large) 64L * 1024 * 1024 else 1024)
        val expected = ByteArray(262144) { (it % 251).toByte() }
        val hash = if (large) "228f3903446aeb88363ccbf444258f060ac18c5653d978b4bb2f5b04956c757c" else
            MessageDigest.getInstance("SHA-256").digest(expected).joinToString("") { "%02x".format(it.toInt() and 255) }
        val providerCalls = AtomicInteger()
        val request = DownloadRequest(ResourceIdentity("process", "resource", runId), RequestInfo(if (large) "$base/m4/large" else "$base/m4/process-$kind"),
            addressReference = "fresh-process-provider", expectedSha256 = hash, requireDigest = true,
            provider = RequestProvider { value -> providerCalls.incrementAndGet(); RequestInfo(value.proposedUrl) })
        val readyFile = File(context.filesDir, "m4-process-$kind-ready.json")
        val result = JSONObject().put("run_id", runId).put("kind", kind).put("phase", phase).put("passed", false)
            .put("pid", Process.myPid())
        val engine = FetchKit.open(config).await()
        var passed = false
        try {
            if (phase == "prepare") {
                val sub = engine.enqueue(request).await()
                val target = if (large) (1L shl 32) + 32768 else 1024L
                val deadline = SystemClock.elapsedRealtime() + if (large) 3600000 else 15000
                var snapshot = sub.query().await()
                while (snapshot.committedOffset < target && SystemClock.elapsedRealtime() < deadline) {
                    SystemClock.sleep(10); snapshot = sub.query().await()
                }
                assertEquals(TaskState.Running, snapshot.state)
                assertTrue(snapshot.committedOffset >= target)
                readyFile.writeText(JSONObject().put("run_id", runId).put("pid", Process.myPid())
                    .put("committed_offset", snapshot.committedOffset).put("persistent_id", snapshot.persistentId).toString())
                val trigger = File(context.filesDir, "m4-process-$kind-$runId-signal")
                repeat(300) {
                    if (trigger.exists()) {
                        // SIGKILL cannot run finally/close; the controller writes this only after the durable barrier.
                        Process.killProcess(Process.myPid())
                        error("SIGKILL returned")
                    }
                    SystemClock.sleep(100)
                }
                error("Controller did not terminate prepared process")
            }
            val ready = JSONObject(readyFile.readText())
            assertEquals(runId, ready.getString("run_id"))
            val stored = engine.listTasks().await().single()
            assertEquals(TaskState.Paused, stored.snapshot.state)
            assertEquals(ready.getLong("committed_offset"), stored.snapshot.committedOffset)
            val sub = engine.enqueue(request).await()
            val done = sub.awaitStopped().await(if (large) 7200 else 45)
            assertEquals(TaskState.Completed, done.state)
            assertEquals(Integrity.DigestVerified, done.integrity)
            assertTrue(providerCalls.get() > 0)
            val lease = engine.acquire(done.persistentId).await()
            try {
                val actual = MessageDigest.getInstance("SHA-256")
                var offset = 0L
                while (true) {
                    val block = lease.read(offset).await()
                    if (block.isEmpty()) break
                    actual.update(block); offset += block.size
                }
                val actualHash = actual.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
                assertEquals(hash, actualHash)
                assertEquals(if (large) (1L shl 32) + 65539 else expected.size.toLong(), offset)
                result.put("bytes", offset).put("restored_committed_offset", stored.snapshot.committedOffset)
                    .put("sha256", actualHash).put("integrity", done.integrity.name)
            } finally { lease.releaseAsync().await() }
            passed = true
            result.put("passed", true)
        } finally {
            engine.closeAsync().await()
            if (phase == "restore") File(context.filesDir, "m4-process-$kind-result.json").writeText(result.toString(2))
            if (passed) { assertEquals(context.filesDir.canonicalFile, root.canonicalFile.parentFile); root.deleteRecursively() }
        }
    }
}
