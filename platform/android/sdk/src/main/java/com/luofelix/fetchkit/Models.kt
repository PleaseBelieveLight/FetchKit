package com.luofelix.fetchkit

import java.io.File

enum class Status {
    Ok, InvalidArgument, NotSupported, Closed, Busy, Network, Timeout, HttpStatus,
    AuthRequired, ProtocolMismatch, IntegrityMismatch, DiskFull, PermissionDenied, IoError,
    Cancelled, QuotaExceeded, OutOfMemory, InternalError, CertificateError, NotFound,
    UnsupportedMetadata, SubscriptionOverflow;
    companion object { internal fun decode(value: String) = entries.getOrElse(value.toInt()) { InternalError } }
}

class FetchKitException(val status: Status) : RuntimeException("FetchKit: $status")

enum class TaskState {
    Queued, Running, Paused, TransferComplete, Failed, Cancelled, Pausing,
    Verifying, Publishing, Completed, RetryWaiting, Removed;
    val stopped: Boolean get() = this in setOf(Paused, TransferComplete, Failed, Cancelled, Completed, Removed)
}
enum class Integrity { None, TransportComplete, DigestVerified }

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

data class ResourceIdentity(val namespace: String, val key: String, val version: String, val representation: String = "")
data class RequestInfo(val url: String, val headers: Map<String, String> = emptyMap())
enum class RequestReason { Initial, Redirect, AuthRefresh }
class RequestContext internal constructor(
    val reason: RequestReason, val taskId: Long, val generation: Long, val httpStatus: Int,
    val proposedUrl: String, val previousOrigin: String, val addressReference: String,
    private val cancelled: () -> Boolean,
) { fun isCancelled(): Boolean = cancelled() }
fun interface RequestProvider { fun provide(context: RequestContext): RequestInfo }

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
    internal fun texts(): Array<String> = arrayOf(identity.namespace, identity.key, identity.version,
        identity.representation, addressReference, request.url, expectedSha256) +
        request.headers.flatMap { listOf(it.key, it.value) }
    internal fun numbers() = longArrayOf(if (expectedLength != null) 1 else 0, expectedLength ?: 0,
        if (requireDigest) 1 else 0, expiresAtMs, if (pinOnCreate) 1 else 0)
}

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
data class StoredTask(val snapshot: TaskSnapshot, val identity: ResourceIdentity, val addressReference: String)
data class StorageStats(val recordCount: Int, val quarantinedRecords: Int, val unsupportedRecords: Int,
    val orphanFiles: Int, val capacityBytes: Long, val usedBytes: Long, val reservedBytes: Long,
    val chargedBytes: Long, val metadataBudgetBytes: Long)
enum class EventKind { State, Progress, SubscriptionError }
data class TaskEvent(val kind: EventKind, val taskId: Long, val generation: Long, val state: TaskState,
    val error: Status, val receivedBytes: Long, val committedOffset: Long, val totalBytes: Long?,
    val integrity: Integrity, val httpStatus: Int, val retryCount: Int, val nextRetryAtMs: Long) {
    companion object {
        internal fun decode(v: List<String>) = TaskEvent(EventKind.entries[v[0].toInt()], v[1].toLong(),
            v[2].toLong(), TaskState.entries[v[3].toInt()], Status.decode(v[4]), v[5].toLong(), v[6].toLong(),
            v[7].toLong().takeIf { v[8] == "1" }, Integrity.entries[v[9].toInt()], v[10].toInt(), v[11].toInt(), v[12].toLong())
    }
}
