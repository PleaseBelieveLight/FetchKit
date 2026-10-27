package com.luofelix.fetchkit.sample

import android.content.Context
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.luofelix.fetchkit.*
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.CompletableFuture
import java.util.concurrent.Executor

/** Host-owned execution permission; the SDK has no Activity/JobScheduler dependency. */
internal object SampleDownloads {
    val main = Executor { Handler(Looper.getMainLooper()).post(it) }
    var rows by mutableStateOf<List<StoredTask>>(emptyList())
        private set
    var message by mutableStateOf("")
    var visible = false
    var jobRunning = false
    private var current: CompletableFuture<DownloadEngine>? = null
    private var previousClose = CompletableFuture.completedFuture(Unit)
    private val subscriptions = java.util.concurrent.ConcurrentHashMap<ResourceIdentity, DownloadSubscription>()

    @Synchronized fun engine(context: Context, network: android.net.Network? = null): CompletableFuture<DownloadEngine> {
        return current ?: previousClose.thenCompose {
            FetchKit.open(EngineConfig(File(context.applicationContext.filesDir, "fetchkit"),
                2L * 1024 * 1024 * 1024, 4L * 1024 * 1024 * 1024, 7L * 86400000,
                allowHttp = (context.applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0, network = network))
        }.also { current = it }
    }
    @Synchronized fun jobEngine(context: Context, network: android.net.Network): CompletableFuture<DownloadEngine> {
        stop()
        return engine(context, network)
    }
    @Synchronized fun stop(): CompletableFuture<Unit> {
        val engine = current ?: return previousClose
        current = null
        subscriptions.clear()
        previousClose = engine.thenCompose { it.closeAsync() }
        return previousClose
    }
    fun releasePermission() { if (!visible && !jobRunning) stop() }
    fun bind(context: Context, request: DownloadRequest, start: Boolean): CompletableFuture<DownloadSubscription> =
        engine(context).thenCompose { if (start) it.enqueue(request) else it.attach(request) }.thenApply { sub ->
            subscriptions.put(request.identity, sub)?.releaseAsync()
            sub
        }
    fun refresh(context: Context): CompletableFuture<Unit> = engine(context).thenCompose { it.listTasks() }
        .thenAcceptAsync({ rows = it }, main).thenApply { Unit }
    fun request(url: String, digest: String, version: String, pinned: Boolean): DownloadRequest {
        val key = MessageDigest.getInstance("SHA-256").digest(url.toByteArray()).joinToString("") { "%02x".format(it.toInt() and 255) }
        return DownloadRequest(ResourceIdentity("sample", key, version), RequestInfo(url),
            addressReference = "sample-public-resource", expectedSha256 = digest.trim(),
            requireDigest = digest.isNotBlank(), pinOnCreate = pinned)
    }
    fun showError(error: Throwable?) {
        if (error != null) main.execute {
            var cause = error
            while (cause?.cause != null) cause = cause.cause
            message = if (cause is FetchKitException) "操作失败：${cause.status}" else "请检查输入或执行环境"
        }
    }
}
