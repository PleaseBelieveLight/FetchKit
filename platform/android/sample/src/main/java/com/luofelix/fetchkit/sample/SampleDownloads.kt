package com.luofelix.fetchkit.sample

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.AtomicFile
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.luofelix.fetchkit.*
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executor
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit

/** 管理宿主执行许可与展示状态；下载和存储策略由 SDK 核心负责。 */
internal object SampleDownloads {
    init {
        // 示例同时覆盖前台和 JobService 入口；正式宿主按需开启，SDK 自身默认关闭。
        FetchKit.setLogLevel(LogLevel.Debug)
    }
    val main = Executor { Handler(Looper.getMainLooper()).post(it) }
    private val io = ThreadPoolExecutor(1, 1, 30, TimeUnit.SECONDS, ArrayBlockingQueue(64),
        { Thread(it, "sample-requests").apply { isDaemon = true } }, ThreadPoolExecutor.AbortPolicy())
    var rows by mutableStateOf<List<StoredTask>>(emptyList()); private set
    var stats by mutableStateOf<StorageStats?>(null); private set
    var speeds by mutableStateOf<Map<String, Long>>(emptyMap()); private set
    var events by mutableStateOf<List<String>>(emptyList()); private set
    var message by mutableStateOf("")
    var busy by mutableStateOf(false); private set
    var leasedId by mutableStateOf<String?>(null); private set
    var readPreview by mutableStateOf(""); private set
    var visible = false
    var jobRunning = false
    var displayLimit by mutableStateOf(100); private set
    private var current: CompletableFuture<DownloadEngine>? = null
    private var previousClose = CompletableFuture.completedFuture(Unit)
    @Volatile private var epoch = 0L
    private var lastSample = 0L
    private var lease: ResourceLease? = null
    private val requests = ConcurrentHashMap<ResourceIdentity, DownloadRequest>()
    private data class Binding(val subscription: DownloadSubscription, val observation: AutoCloseable)
    private val subscriptions = ConcurrentHashMap<ResourceIdentity, CopyOnWriteArrayList<Binding>>()

    @Synchronized fun engine(context: Context, network: android.net.Network? = null): CompletableFuture<DownloadEngine> {
        val app = context.applicationContext
        return current ?: previousClose.thenComposeAsync({
            loadPublicRequests(app)
            FetchKit.open(EngineConfig(File(app.filesDir, "fetchkit"), 2L shl 30, 4L shl 30, 7L * 86400000,
                allowHttp = (app.applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0,
                network = network))
        }, io).also { opened ->
            current = opened
            opened.whenCompleteAsync({ _, error ->
                if (error != null) synchronized(this) { if (current === opened) current = null }
            }, main)
        }
    }

    @Synchronized fun jobEngine(context: Context, network: android.net.Network): CompletableFuture<DownloadEngine> {
        stop()
        return engine(context, network)
    }

    @Synchronized fun stop(): CompletableFuture<Unit> {
        epoch++
        val opened = current
        val preceding = previousClose
        current = null
        subscriptions.values.flatten().forEach { it.observation.close() }
        subscriptions.clear()
        val held = lease
        lease = null
        main.execute { leasedId = null; readPreview = ""; speeds = emptyMap(); lastSample = 0 }
        val released = held?.releaseAsync() ?: CompletableFuture.completedFuture(Unit)
        previousClose = released.thenCompose {
            opened?.handle { engine, _ -> engine }?.thenCompose { it?.closeAsync() ?: CompletableFuture.completedFuture(Unit) }
                ?: preceding
        }
        return previousClose
    }

    fun releasePermission() { if (!visible && !jobRunning) stop() }

    /** Adding an independent subscription is explicit; controls reuse the row's own request. */
    fun bind(context: Context, request: DownloadRequest, start: Boolean): CompletableFuture<DownloadSubscription> {
        val existing = subscriptions[request.identity]?.firstOrNull()?.subscription
        if (existing != null) {
            val old = requests[request.identity]
            if (old != null && (old.expectedLength != request.expectedLength || old.expectedSha256 != request.expectedSha256 ||
                    old.requireDigest != request.requireDigest || old.addressReference != request.addressReference))
                return failed(FetchKitException(Status.InvalidArgument))
            if (!start) return CompletableFuture.completedFuture(existing)
            return existing.query().thenCompose { snapshot ->
                if (snapshot.canResume) existing.resume().thenApply { existing }
                else if (snapshot.state == TaskState.Completed && !snapshot.available) {
                    // Expired/evicted objects must be explicitly enqueued, not resumed on a historical token.
                    subscriptions.remove(request.identity).orEmpty().fold(CompletableFuture.completedFuture(Unit)) { future, binding ->
                        future.thenCompose { binding.observation.close(); binding.subscription.releaseAsync() }
                    }.thenCompose {
                        addSubscription(context, request, true)
                    }
                } else CompletableFuture.completedFuture(existing)
            }
        }
        return addSubscription(context, request, start)
    }

    private fun addSubscription(context: Context, request: DownloadRequest, start: Boolean): CompletableFuture<DownloadSubscription> {
        val permission = epoch
        return engine(context).thenComposeAsync({ engine ->
            if (start) engine.enqueue(request) else engine.attach(request)
        }, io).thenComposeAsync({ sub ->
            synchronized(this) {
                if (epoch != permission) return@thenComposeAsync sub.releaseAsync().thenCompose<DownloadSubscription> {
                    failed(FetchKitException(Status.Closed))
                }
                val observation = sub.observe(main) { event ->
                    if (epoch == permission && event.kind != EventKind.Progress) {
                        val time = SimpleDateFormat("HH:mm:ss", Locale.ROOT).format(Date())
                        events = (listOf("$time · #${event.taskId} · ${event.state.label()} · ${event.error}") + events).take(60)
                    }
                }
                subscriptions.computeIfAbsent(request.identity) { CopyOnWriteArrayList() }.add(Binding(sub, observation))
                requests[request.identity] = request
            }
            savePublicRequests(context)
            CompletableFuture.completedFuture(sub)
        }, io)
    }

    fun requestFor(row: StoredTask): DownloadRequest? = requests[row.identity]
    fun title(row: StoredTask): String = downloadPresets.firstOrNull { resourceKey(it.url) == row.identity.key }?.title
        ?: "自定义资源 · ${row.identity.key.take(8)}"
    fun subscriptionCount(row: StoredTask): Int = subscriptions[row.identity]?.size ?: 0

    fun start(context: Context, selected: List<DownloadRequest>) = perform(context, "已提交 ${selected.size} 个文件") {
        selected.fold(CompletableFuture.completedFuture(Unit)) { future, request ->
            future.thenCompose { bind(context, request, true).thenApply { Unit } }
        }
    }

    fun control(context: Context, row: StoredTask, action: String) = perform(context, when (action) {
        "pause" -> "已暂停，检查点已保存"
        "resume" -> "已恢复任务；续传方式由服务端校验结果决定"
        "subscribe" -> "已增加独立订阅，共享同一个下载任务"
        "cancel" -> "已取消一个订阅；最后一个订阅取消才清理未完成数据"
        else -> "已释放一个订阅；最后一个释放会保留检查点"
    }) {
        val request = requestFor(row) ?: return@perform failed<Unit>(IllegalArgumentException("请重新提供此任务的地址和原校验参数"))
        when (action) {
            "subscribe" -> addSubscription(context, request, false).thenApply { Unit }
            "resume" -> bind(context, request, true).thenApply { Unit }
            "pause" -> bind(context, request, false).thenCompose { it.pause() }.thenApply { Unit }
            else -> {
                val bindings = subscriptions[row.identity]
                val binding = bindings?.lastOrNull()
                    ?: return@perform failed<Unit>(IllegalArgumentException("此任务没有本页面持有的订阅，请先恢复或添加订阅"))
                val done = if (action == "cancel") binding.subscription.cancel().thenApply { Unit }
                    else CompletableFuture.completedFuture(Unit)
                done.thenCompose { binding.observation.close(); binding.subscription.releaseAsync() }.thenApply {
                    bindings.remove(binding); Unit
                }
            }
        }
    }

    fun pauseAll(context: Context) = perform(context, "已暂停全部前台任务并保存检查点") {
        rows.filter { !it.snapshot.state.stopped }.mapNotNull { subscriptions[it.identity]?.firstOrNull()?.subscription }
            .fold(CompletableFuture.completedFuture(Unit)) { future, sub -> future.thenCompose { sub.pause().thenApply { Unit } } }
    }

    fun resumeAll(context: Context) = start(context, rows.filter {
        it.snapshot.canResume
    }.mapNotNull(::requestFor))

    fun refresh(context: Context): CompletableFuture<Unit> {
        val permission = epoch
        val limit = displayLimit
        fun pages(engine: DownloadEngine, offset: Int, result: List<StoredTask>): CompletableFuture<List<StoredTask>> =
            engine.listTasks(offset, 100).thenCompose { page ->
                val combined = result + page
                if (page.size == 100 && combined.size < limit) pages(engine, offset + 100, combined)
                else CompletableFuture.completedFuture(combined)
            }
        return engine(context).thenCompose { engine ->
            pages(engine, 0, emptyList()).thenCombine(engine.storageStats()) { tasks, storage -> tasks to storage }
        }.thenAcceptAsync({ (tasks, storage) ->
            if (permission == epoch) {
                val now = SystemClock.elapsedRealtime()
                val before = rows.associateBy { it.snapshot.persistentId }
                speeds = tasks.associate { row ->
                    val old = before[row.snapshot.persistentId]?.snapshot
                    val delta = if (old?.generation == row.snapshot.generation && row.snapshot.state == TaskState.Running)
                        (row.snapshot.receivedBytes - old.receivedBytes).coerceAtLeast(0) else 0
                    row.snapshot.persistentId to if (lastSample > 0 && now > lastSample) delta * 1000 / (now - lastSample) else 0L
                }
                lastSample = now
                rows = tasks
                stats = storage
            }
        }, main).thenApply { Unit }
    }

    fun loadMore(context: Context) { displayLimit += 100; refresh(context).whenComplete { _, error -> showError(error) } }
    fun pin(context: Context, row: StoredTask) = perform(context, "已更新保留策略") {
        engine(context).thenCompose { it.pin(row.snapshot.persistentId, !row.snapshot.pinned) }
    }
    fun expiry(context: Context, row: StoredTask, time: Long) = perform(context, "已更新过期策略") {
        engine(context).thenCompose { it.setExpiry(row.snapshot.persistentId, time) }
    }
    fun remove(context: Context, row: StoredTask) = perform(context, "已移除资源") {
        val bindings = subscriptions.remove(row.identity).orEmpty()
        bindings.fold(CompletableFuture.completedFuture(Unit)) { future, binding ->
            future.thenCompose { binding.observation.close(); binding.subscription.releaseAsync() }
        }.thenCompose { engine(context) }.thenCompose { it.remove(row.snapshot.persistentId) }
            .thenRunAsync({ requests.remove(row.identity); savePublicRequests(context) }, io).thenApply { Unit }
    }
    fun trim(context: Context) = perform(context, "已尝试按 LRU 回收至 512 MiB；保留、租约和活动任务受保护") {
        engine(context).thenCompose { it.trimTo(512L * 1024 * 1024) }
    }
    fun collect(context: Context) = perform(context, "已清理超过 7 天且未受保护的暂存数据") {
        engine(context).thenCompose { it.collectStaging() }
    }

    fun read(context: Context, row: StoredTask) = perform(context, "已读取前 64 字节，可在存储页查看并释放租约") {
        val permission = epoch
        releaseLease().thenCompose { engine(context) }.thenCompose { it.acquire(row.snapshot.persistentId) }
            .thenComposeAsync({ acquired ->
                if (permission == epoch) {
                    lease = acquired; leasedId = row.snapshot.persistentId
                    acquired.read(0, 64)
                } else acquired.releaseAsync().thenCompose<ByteArray> { failed(FetchKitException(Status.Closed)) }
            }, main).thenAcceptAsync({ bytes ->
                readPreview = bytes.joinToString(" ") { "%02x".format(it.toInt() and 255) }.ifEmpty { "空文件（0 字节）" }
            }, main).thenApply { Unit }
    }
    fun releaseRead(context: Context) = perform(context, "已释放读取租约") { releaseLease() }
    private fun releaseLease(): CompletableFuture<Unit> {
        val held = lease
        lease = null
        main.execute { leasedId = null; readPreview = "" }
        return held?.releaseAsync() ?: CompletableFuture.completedFuture(Unit)
    }

    fun perform(context: Context, success: String, operation: () -> CompletableFuture<*>) {
        if (busy) return
        busy = true
        val result = try { operation() } catch (error: Throwable) { failed<Unit>(error) }
        result.whenCompleteAsync({ _, error ->
            busy = false
            if (error == null) message = success else showError(error)
            if (visible || jobRunning) refresh(context).whenComplete { _, refreshError -> showError(refreshError) }
        }, main)
    }

    fun request(url: String, digest: String, version: String, pinned: Boolean): DownloadRequest = sampleRequest(url, digest, version, pinned)
    fun showError(error: Throwable?) {
        if (error == null) return
        var cause = error
        while (cause?.cause != null) cause = cause.cause
        val root = cause
        main.execute { message = when (root) {
            is FetchKitException -> "操作失败：${root.status}。${when (root.status) {
                Status.QuotaExceeded -> "空间不足或存在受保护资源，可取消保留并释放租约后再试。"
                Status.Busy -> "资源正在使用，请等待任务停止或释放租约。"
                Status.IntegrityMismatch -> "长度或 SHA-256 不符，文件未发布。"
                else -> "可在任务详情查看 HTTP 状态和重试信息。"
            }}"
            is IllegalArgumentException -> root.message ?: "请检查输入"
            else -> "操作未完成，请检查网络和执行环境"
        } }
    }

    private fun <T> failed(error: Throwable): CompletableFuture<T> = CompletableFuture<T>().apply { completeExceptionally(error) }
    private fun catalogFile(context: Context) = AtomicFile(File(context.noBackupFilesDir, "sample-public-requests.json"))
    private fun loadPublicRequests(context: Context) {
        val file = catalogFile(context)
        if (!file.baseFile.exists()) return
        try {
            val json = JSONArray(file.openRead().use { it.readBytes().toString(Charsets.UTF_8) })
            repeat(json.length()) { index ->
                val entry = json.getJSONObject(index)
                val preset = downloadPresets.firstOrNull { it.id == entry.getString("preset") } ?: return@repeat
                val request = preset.request(entry.getString("version"), entry.optString("digest"), entry.optBoolean("pin"))
                    .copy(expectedLength = if (entry.has("length")) entry.getLong("length") else null)
                requests.putIfAbsent(request.identity, request)
            }
        } catch (_: Exception) { main.execute { message = "预设配置读取失败；请重新选择资源并提供原校验参数。下载记录仍由 SDK 保留。" } }
    }
    private fun savePublicRequests(context: Context) {
        val json = JSONArray()
        requests.values.forEach { request ->
            val preset = downloadPresets.firstOrNull { request.addressReference == "sample-preset:${it.id}" &&
                request.request.url == it.url && request.identity.key == resourceKey(it.url) } ?: return@forEach
            val entry = JSONObject().put("preset", preset.id).put("version", request.identity.version)
                .put("digest", request.expectedSha256).put("pin", request.pinOnCreate)
            request.expectedLength?.let { entry.put("length", it) }
            json.put(entry)
        }
        val file = catalogFile(context)
        val output = file.startWrite()
        try { output.write(json.toString().toByteArray(Charsets.UTF_8)); file.finishWrite(output) }
        catch (error: Throwable) { file.failWrite(output); throw error }
    }
}
