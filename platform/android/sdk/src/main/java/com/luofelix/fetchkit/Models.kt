package com.luofelix.fetchkit

import java.io.File

/** 跨 C/JNI/Kotlin 的错误分类；枚举顺序与 native 编码一致，不能单独重排。 */
enum class Status {
    Ok, InvalidArgument, NotSupported, Closed, Busy, Network, Timeout, HttpStatus,
    AuthRequired, ProtocolMismatch, IntegrityMismatch, DiskFull, PermissionDenied, IoError,
    Cancelled, QuotaExceeded, OutOfMemory, InternalError, CertificateError, NotFound,
    UnsupportedMetadata, SubscriptionOverflow;
    companion object { internal fun decode(value: String) = entries.getOrElse(value.toInt()) { InternalError } }
}

/** API 调用失败；仅暴露安全错误码，不包含 URL、凭证或平台异常原文。 */
class FetchKitException(val status: Status) : RuntimeException("FetchKit: $status")

/** 共享任务状态；订阅撤销也可呈现 Cancelled。stopped 只表示等待结束，不代表下载成功。 */
enum class TaskState {
    Queued, Running, Paused, TransferComplete, Failed, Cancelled, Pausing,
    Verifying, Publishing, Completed, RetryWaiting, Removed;
    val stopped: Boolean get() = this in setOf(Paused, TransferComplete, Failed, Cancelled, Completed, Removed)
}
/** None 未完成校验；TransportComplete 仅传输/长度一致；DigestVerified 才表示可信 SHA-256 匹配。 */
enum class Integrity { None, TransportComplete, DigestVerified }

/**
 * 引擎配置；所有容量/偏移为字节，超时/保留期为毫秒，前四项须由宿主明确设置（数值为正）。
 * storageRoot 须为私有且排除备份的目录；存储容量包含元数据、孤立文件与空间预留。
 * maxActiveDownloads 为 1～16，maxQueuedTasks 为 1～1024；maxRetries 为额外尝试次数（0～16）。
 * checkpointBytes/IntervalMs 任一达到即尝试检查点，时间条件在收到数据时检查。
 * providerTimeoutMs 是协作截止时间，不能强制中断任意宿主代码；network 同时约束 socket 与 DNS。
 */
data class EngineConfig(
    val storageRoot: File,
    val maxDownloadBytes: Long,
    val storageCapacityBytes: Long,
    val stagingRetentionMs: Long,
    val allowHttp: Boolean = false,
    val maxActiveDownloads: Int = 3,
    val maxQueuedTasks: Int = 1024,
    val maxRetries: Int = 3,
    val connectTimeoutMs: Int = 5000,
    val readTimeoutMs: Int = 15000,
    val providerTimeoutMs: Int = 10000,
    val maxRedirects: Int = 5,
    val maxRequests: Int = 4096,
    val checkpointBytes: Long = 4L * 1024 * 1024,
    val checkpointIntervalMs: Int = 1000,
    val retryBaseDelayMs: Int = 1000,
    val retryMaxDelayMs: Int = 30000,
    val network: android.net.Network? = null,
) {
    init {
        require(maxDownloadBytes > 0 && storageCapacityBytes > 0 && stagingRetentionMs > 0)
        require(maxActiveDownloads in 1..16 && maxQueuedTasks in 1..1024 && maxRetries in 0..16)
        require(connectTimeoutMs in 1..300000 && readTimeoutMs in 1..300000 && providerTimeoutMs in 1..60000)
        require(maxRedirects in 0..32 && maxRequests in 1..65536 && checkpointBytes > 0 && checkpointIntervalMs in 1..300000)
        require(retryBaseDelayMs in 1..30000 && retryMaxDelayMs in retryBaseDelayMs..30000)
    }
    internal fun numbers() = longArrayOf(maxDownloadBytes, storageCapacityBytes, stagingRetentionMs,
        if (allowHttp) 1 else 0, maxActiveDownloads.toLong(), maxQueuedTasks.toLong(), maxRetries.toLong(),
        connectTimeoutMs.toLong(), readTimeoutMs.toLong(), providerTimeoutMs.toLong(), maxRedirects.toLong(),
        maxRequests.toLong(), checkpointBytes, checkpointIntervalMs.toLong(), retryBaseDelayMs.toLong(), retryMaxDelayMs.toLong())
}

/** 资源身份四元组，区分账号/业务域、资源版本和表示；URL/凭证不参与身份。 */
data class ResourceIdentity(val namespace: String, val key: String, val version: String, val representation: String = "")
/** 短期请求信息；不写入 SDK 记录，跨 origin 的凭证转交由核心校验。toString 隐藏所有外部字符串。 */
data class RequestInfo(val url: String, val headers: Map<String, String> = emptyMap()) {
    override fun toString(): String = "RequestInfo(url=<已隐藏>, headerCount=${headers.size})"
}
/** 请求 provider 的调用原因：初始请求、重定向或一次鉴权刷新。 */
enum class RequestReason { Initial, Redirect, AuthRefresh }
/** provider 本次调用的上下文；不得保存 proposedUrl 等敏感值到日志或持久化记录。 */
class RequestContext internal constructor(
    val reason: RequestReason, val taskId: Long, val generation: Long, val httpStatus: Int,
    val proposedUrl: String, val previousOrigin: String, val addressReference: String,
    private val cancelled: () -> Boolean,
) {
    /** 仅用于当前 provider 的协作取消；调用范围结束后返回 true，不能复用 token 控制后续请求。 */
    fun isCancelled(): Boolean = cancelled()
}
/** 在 native worker 上获取最新请求；需协作检查取消，禁止阻塞等待 engine API Future。异常映射为 AuthRequired。 */
fun interface RequestProvider { fun provide(context: RequestContext): RequestInfo }

/**
 * 下载约束与临时请求。expectedLength=null 表示未知，0 表示空文件；摘要为空或可信的 64 位 SHA-256 十六进制串。
 * requireDigest 要求提供可信摘要；同身份重新绑定必须保持原长度/摘要/业务引用约束。
 * expiresAtMs 为 UTC Unix 毫秒，0 表示不过期；pinOnCreate 在首次准入时原子持久化。
 * provider、URL 和 headers 不持久化，恢复时须由宿主重新提供。toString 仅输出安全的数值/布尔约束。
 */
data class DownloadRequest(
    val identity: ResourceIdentity,
    val request: RequestInfo = RequestInfo(""),
    val addressReference: String = "",
    val expectedLength: Long? = null,
    val expectedSha256: String = "",
    val requireDigest: Boolean = false,
    val expiresAtMs: Long = 0,
    val pinOnCreate: Boolean = false,
    val provider: RequestProvider? = null,
) {
    init { require(expectedLength == null || expectedLength >= 0); require(expiresAtMs >= 0) }
    override fun toString(): String = "DownloadRequest(request=<已隐藏>, expectedLength=$expectedLength, " +
        "requireDigest=$requireDigest, expiresAtMs=$expiresAtMs, pinOnCreate=$pinOnCreate, hasProvider=${provider != null})"
    internal fun texts(): Array<String> = arrayOf(identity.namespace, identity.key, identity.version,
        identity.representation, addressReference, request.url, expectedSha256) +
        request.headers.flatMap { listOf(it.key, it.value) }
    internal fun numbers() = longArrayOf(if (expectedLength != null) 1 else 0, expectedLength ?: 0,
        if (requireDigest) 1 else 0, expiresAtMs, if (pinOnCreate) 1 else 0)
}

/**
 * 不可变任务快照；receivedBytes 可领先于已持久化的 committedOffset，恢复时只信任后者。
 * totalBytes=null 表示总长未知；taskId 是订阅 token（未绑定的列表项为 0），persistentId 才是跨进程身份。
 * generation 区分运行轮次，retryCount 为已使用的额外重试次数；所有 AtMs 字段均为 UTC Unix 毫秒。
 * available 还受过期/淘汰影响；Completed 不等同于 DigestVerified，读取仍须 acquire。
 */
data class TaskSnapshot(
    val taskId: Long, val generation: Long, val state: TaskState, val error: Status,
    val receivedBytes: Long, val committedOffset: Long, val totalBytes: Long?,
    val integrity: Integrity, val httpStatus: Int, val platformCode: Int, val canResume: Boolean,
    val requestCount: Int, val retryCount: Int, val subscriberCount: Int, val nextRetryAtMs: Long,
    val expiresAtMs: Long, val available: Boolean, val pinned: Boolean, val persistentId: String,
) {
    companion object {
        internal const val WIDTH = 20
        internal fun decode(v: List<String>): TaskSnapshot {
            require(v.size == WIDTH)
            return TaskSnapshot(v[0].toLong(), v[1].toLong(), TaskState.entries[v[2].toInt()], Status.decode(v[3]),
                v[4].toLong(), v[5].toLong(), v[6].toLong().takeIf { v[7] == "1" }, Integrity.entries[v[8].toInt()],
                v[9].toInt(), v[10].toInt(), v[11] == "1", v[12].toInt(), v[13].toInt(), v[14].toInt(),
                v[15].toLong(), v[16].toLong(), v[17] == "1", v[18] == "1", v[19])
        }
    }
}
/** 恢复列表项；业务引用不包含已保存的请求凭证，也不意味着已恢复执行许可。 */
data class StoredTask(val snapshot: TaskSnapshot, val identity: ResourceIdentity, val addressReference: String)
/** 逻辑字节账本；chargedBytes 包含预留与元数据预算，未知归属文件保留并计费。 */
data class StorageStats(val recordCount: Int, val quarantinedRecords: Int, val unsupportedRecords: Int,
    val orphanFiles: Int, val capacityBytes: Long, val usedBytes: Long, val reservedBytes: Long,
    val chargedBytes: Long, val metadataBudgetBytes: Long)
/** 状态与终态不合并；进度可合并；SubscriptionError 仅终止对应订阅。 */
enum class EventKind { State, Progress, SubscriptionError }
/** 有界事件快照；宿主以 taskId/generation 关联业务状态，不包含 URL、headers 或文件路径。 */
data class TaskEvent(val kind: EventKind, val taskId: Long, val generation: Long, val state: TaskState,
    val error: Status, val receivedBytes: Long, val committedOffset: Long, val totalBytes: Long?,
    val integrity: Integrity, val httpStatus: Int, val retryCount: Int, val nextRetryAtMs: Long) {
    companion object {
        internal fun decode(v: List<String>) = TaskEvent(EventKind.entries[v[0].toInt()], v[1].toLong(),
            v[2].toLong(), TaskState.entries[v[3].toInt()], Status.decode(v[4]), v[5].toLong(), v[6].toLong(),
            v[7].toLong().takeIf { v[8] == "1" }, Integrity.entries[v[9].toInt()], v[10].toInt(), v[11].toInt(), v[12].toLong())
    }
}
