package com.luofelix.fetchkit.sample

import android.os.Build
import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.luofelix.fetchkit.probe.PlatformProbe
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.net.Authenticator
import java.net.CookieHandler
import java.net.PasswordAuthentication
import java.net.URI
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.ceil

@RunWith(AndroidJUnit4::class)
class PlatformProbeTest {
    private fun fingerprint(bytes: ByteArray): Long {
        var value = -3750763034362895579L
        bytes.forEach { value = (value xor (it.toLong() and 255)) * 1099511628211L }
        return value
    }

    @Test
    fun measureTransportContract() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val arguments = InstrumentationRegistry.getArguments()
        val baseUrl = requireNotNull(arguments.getString("fetchkitProbeUrl"))
        val repetitions = arguments.getString("fetchkitProbeRepetitions")?.toInt() ?: 100
        require(repetitions in 1..1000)
        val fixture = instrumentation.context.assets.open("m0_scenarios.json").bufferedReader().use { it.readText() }
        val scenarios = JSONObject(fixture).getJSONArray("scenarios")
        val samples = JSONArray()
        val failures = mutableListOf<String>()
        val summary = JSONObject()
        val report = JSONObject().put("backend", PlatformProbe.backendName())
            .put("run_id", arguments.getString("fetchkitProbeRunId"))
            .put("api", Build.VERSION.SDK_INT).put("abi", Build.SUPPORTED_ABIS[0])
            .put("model", Build.MODEL).put("fingerprint", Build.FINGERPRINT)
            .put("samples", samples).put("summary", summary)

        try {
            for (index in 0 until scenarios.length()) {
                val scenario = scenarios.getJSONObject(index)
                val id = scenario.getString("id")
                val cancels = scenario.optBoolean("cancel")
                val timings = mutableListOf<Double>()
                repeat(if (cancels) repetitions else 1) { iteration ->
                    val headers = if (id == "range") mapOf("Range" to "bytes=1024-") else emptyMap()
                    val operation = PlatformProbe.Operation(baseUrl + scenario.getString("path"), id, headers)
                    val resultHolder = AtomicReference<PlatformProbe.Result>()
                    val worker = Thread({ resultHolder.set(operation.execute()) }, "fetchkit-probe-network")
                    worker.start()
                    var ready = true
                    var cancelMs = -1.0
                    if (cancels) {
                        when (id) {
                            "delayed-headers" -> SystemClock.sleep(30)
                            "header-barrier" -> ready = operation.headersSeen.await(5, TimeUnit.SECONDS)
                            "blocked-read", "backpressure" -> ready = operation.bodySeen.await(5, TimeUnit.SECONDS)
                        }
                        if (id == "blocked-read") SystemClock.sleep(30)
                        val started = SystemClock.elapsedRealtimeNanos()
                        operation.cancel()
                        operation.cancel()
                        worker.join(5000)
                        val cleanupStopped = operation.awaitCleanup(5000)
                        cancelMs = (SystemClock.elapsedRealtimeNanos() - started) / 1_000_000.0
                        if (worker.isAlive || !cleanupStopped) {
                            failures.add("$id/$iteration: worker or cleanup still active")
                            error("Probe worker did not quiesce; abort to avoid accumulating threads")
                        }
                        timings.add(cancelMs)
                    } else {
                        worker.join(10000)
                        if (worker.isAlive) {
                            operation.cancel()
                            worker.join(5000)
                            operation.awaitCleanup(5000)
                            failures.add("$id: worker did not finish")
                        }
                    }
                    val result = resultHolder.get()
                    if (result == null) {
                        failures.add("$id/$iteration: no result")
                        return@repeat
                    }
                    val sample = JSONObject().put("scenario", id).put("iteration", iteration)
                        .put("reason", result.reason).put("status", result.status).put("bytes", result.bytes)
                        .put("fingerprint", java.lang.Long.toUnsignedString(result.fingerprint, 16))
                        .put("header_calls", result.headerCalls).put("body_calls", result.bodyCalls)
                        .put("duplicate_headers", result.duplicateHeaders).put("cancel_ms", cancelMs)
                        .put("ready", ready).put("error_type", result.errorType)
                    samples.put(sample)
                    if (!ready || result.headerCalls > 1) failures.add("$id/$iteration: barrier violation")
                    if (cancels) {
                        val completedFirst = id == "close-race" && result.reason == "complete" && result.bytes == 262144L
                        if ((result.reason != "cancelled" && !completedFirst) || cancelMs > 3000) {
                            failures.add("$id/$iteration: cancel bound exceeded or wrong result")
                        }
                    } else {
                        if (result.reason != scenario.optString("result", "complete") ||
                            result.status != scenario.getInt("status") || result.bytes != scenario.getLong("bytes")) {
                            failures.add("$id: status, length or end reason mismatch")
                        }
                        if (id in listOf("reject", "gzip") && result.bodyCalls != 0) {
                            failures.add("$id: rejected body crossed acceptance barrier")
                        }
                        if (id == "headers" && result.duplicateHeaders != 2) failures.add("duplicate headers lost")
                        if (id in listOf("bytes", "headers", "range")) {
                            val start = if (id == "range") 1024 else 0
                            val content = ByteArray(262144 - start) { ((it + start) % 251).toByte() }
                            if (result.fingerprint != fingerprint(content)) failures.add("$id: JNI content differs")
                        }
                    }
                }
                if (timings.isNotEmpty()) {
                    timings.sort()
                    val p95 = timings[ceil(0.95 * timings.size).toInt() - 1]
                    summary.put(id, JSONObject().put("count", timings.size).put("p95_ms", p95).put("max_ms", timings.last()))
                    if (p95 > 1000) failures.add("$id: p95 exceeds 1 second")
                }
            }

            // These globals belong to this isolated instrumentation process only.
            // The production SDK must never replace host-wide handlers.
            val originalCookies = CookieHandler.getDefault()
            try {
                CookieHandler.setDefault(object : CookieHandler() {
                    override fun get(uri: URI, requestHeaders: MutableMap<String, MutableList<String>>): MutableMap<String, MutableList<String>> =
                        mutableMapOf("Cookie" to mutableListOf("fetchkit_m0=synthetic"))
                    override fun put(uri: URI, responseHeaders: MutableMap<String, MutableList<String>>) = Unit
                })
                val cookieResult = PlatformProbe.Operation("$baseUrl/echo", "ambient-cookie").execute()
                val cookieLeaked = cookieResult.fingerprint == fingerprint(
                    "{\"has_authorization\": false, \"has_cookie\": true}".toByteArray())
                report.put("ambient_cookie_sent", cookieLeaked)
                if (cookieLeaked || cookieResult.reason != "complete") {
                    failures.add("HttpURLConnection cannot isolate the host CookieHandler")
                }
            } finally {
                CookieHandler.setDefault(originalCookies)
            }
            val authCalls = AtomicInteger()
            try {
                Authenticator.setDefault(object : Authenticator() {
                    override fun getPasswordAuthentication(): PasswordAuthentication {
                        authCalls.incrementAndGet()
                        return PasswordAuthentication("fetchkit-m0", "synthetic".toCharArray())
                    }
                })
                val authResult = PlatformProbe.Operation("$baseUrl/auth", "ambient-auth").execute()
                report.put("ambient_auth_calls", authCalls.get()).put("ambient_auth_status", authResult.status)
                if (authCalls.get() != 0 || authResult.status != 401) {
                    failures.add("HttpURLConnection performed authentication outside core policy")
                }
            } finally {
                // Android offers setDefault but no portable getter on minSdk 24.
                // This test must run in its own fresh instrumentation process.
                Authenticator.setDefault(null)
            }
        } finally {
            report.put("failures", JSONArray(failures)).put("gate_passed", failures.isEmpty())
            File(instrumentation.targetContext.filesDir, "m0-platform.json").writeText(report.toString(2))
        }
        assertTrue(failures.joinToString("; "), failures.isEmpty())
    }
}
