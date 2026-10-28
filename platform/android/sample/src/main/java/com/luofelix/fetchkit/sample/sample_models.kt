package com.luofelix.fetchkit.sample

import com.luofelix.fetchkit.DownloadRequest
import com.luofelix.fetchkit.RequestInfo
import com.luofelix.fetchkit.ResourceIdentity
import com.luofelix.fetchkit.TaskState
import java.net.URI
import java.security.MessageDigest
import java.util.Locale

// Identify the host application explicitly instead of relying on OkHttp's default UA.
internal const val sampleUserAgent = "FetchKitSample/1.0"

internal data class DownloadPreset(val id: String, val title: String, val size: String, val url: String) {
    fun request(version: String = "v1", digest: String = "", pinned: Boolean = false) =
        sampleRequest(url, digest, version, pinned).copy(addressReference = "sample-preset:$id")
}

internal val downloadPresets = listOf(
    DownloadPreset("tuna-ubuntu-24-04-5-netboot", "清华镜像 · Netboot", "大小未知",
        "https://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/24.04.5/ubuntu-24.04.5-netboot-amd64.tar.gz?utm_source=chatgpt.com"),
    DownloadPreset("tuna-ubuntu-24-04-5-wsl", "清华镜像 · WSL", "大小未知",
        "https://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/24.04.5/ubuntu-24.04.5-wsl-amd64.wsl?utm_source=chatgpt.com"),
    DownloadPreset("aliyun-ubuntu-24-04-5-netboot", "阿里云镜像 · Netboot", "大小未知",
        "https://mirrors.aliyun.com/ubuntu-releases/24.04.5/ubuntu-24.04.5-netboot-amd64.tar.gz?utm_source=chatgpt.com"),
)

internal fun resourceKey(url: String): String = MessageDigest.getInstance("SHA-256")
    .digest(url.toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it.toInt() and 255) }

internal fun sampleRequest(url: String, digest: String, version: String, pinned: Boolean): DownloadRequest {
    val address = URI(url.trim())
    require(address.scheme in setOf("https", "http") && address.host != null &&
        address.rawUserInfo == null && address.rawFragment == null) { "请输入有效的 HTTP(S) 地址，勿包含凭证或片段" }
    require(version.trim().isNotEmpty()) { "业务版本不能为空" }
    require(digest.isBlank() || digest.trim().matches(Regex("[a-fA-F0-9]{64}"))) { "SHA-256 需要 64 位十六进制字符" }
    return DownloadRequest(ResourceIdentity("sample", resourceKey(url.trim()), version.trim()),
        RequestInfo(url.trim(), mapOf("User-Agent" to sampleUserAgent)),
        addressReference = "sample-session", expectedSha256 = digest.trim().lowercase(Locale.ROOT),
        requireDigest = digest.isNotBlank(), pinOnCreate = pinned)
}

internal fun formatBytes(bytes: Long): String {
    if (bytes < 1024) return "$bytes B"
    val units = listOf("KiB", "MiB", "GiB", "TiB")
    var value = bytes.toDouble() / 1024
    var index = 0
    while (value >= 1024 && index < units.lastIndex) { value /= 1024; index++ }
    return String.format(Locale.ROOT, if (value >= 100) "%.0f %s" else "%.1f %s", value, units[index])
}

internal fun TaskState.label(): String = when (this) {
    TaskState.Queued -> "排队中"
    TaskState.Running -> "下载中"
    TaskState.Paused -> "已暂停"
    TaskState.TransferComplete -> "传输完成"
    TaskState.Failed -> "下载失败"
    TaskState.Cancelled -> "已取消"
    TaskState.Pausing -> "保存检查点"
    TaskState.Verifying -> "校验中"
    TaskState.Publishing -> "发布中"
    TaskState.Completed -> "已完成"
    TaskState.RetryWaiting -> "等待重试"
    TaskState.Removed -> "已移除"
}
