package com.luofelix.fetchkit.sample

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.job.JobInfo
import android.app.job.JobParameters
import android.app.job.JobScheduler
import android.app.job.JobService
import android.content.ComponentName
import android.content.Context
import android.os.Build
import android.os.PersistableBundle
import java.net.URI
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import com.luofelix.fetchkit.DownloadRequest
import com.luofelix.fetchkit.FetchKitException
import com.luofelix.fetchkit.Status
import com.luofelix.fetchkit.ResourceIdentity
import java.util.UUID
import java.util.concurrent.CompletableFuture

/** Finite, interruptible regular jobs below API 34; user-initiated jobs on API 34+. */
class DownloadJobService : JobService() {
    private class Run(val params: JobParameters, val request: DownloadRequest) {
        var network: Network? = null
        var attempt = Any()
        var finishing = false
        var callback: ConnectivityManager.NetworkCallback? = null
    }

    private var active: Run? = null
    private val connectivity get() = getSystemService(ConnectivityManager::class.java)

    private fun hasPermission(params: JobParameters): Boolean {
        if (isWithdrawing(params.jobId)) return false
        val scheduled = getSystemService(JobScheduler::class.java).getPendingJob(params.jobId) ?: return false
        return scheduled.extras.getString("permit") == params.extras.getString("permit")
    }

    private fun matches(run: Run, params: JobParameters): Boolean =
        run.params.jobId == params.jobId && run.params.extras.getString("permit") == params.extras.getString("permit")
    override fun onStartJob(params: JobParameters): Boolean {
        if (!hasPermission(params)) return false
        val waitingForEngine = active != null
        if (Build.VERSION.SDK_INT >= 34 && params.isUserInitiatedJob) {
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(NotificationChannel("downloads", "文件下载", NotificationManager.IMPORTANCE_LOW))
            val notification = Notification.Builder(this, "downloads").setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle("FetchKit 下载")
                .setContentText(if (waitingForEngine) "等待其他后台下载释放引擎" else "可在应用内暂停或取消")
                .setOngoing(true).build()
            setNotification(params, params.jobId, notification, JOB_END_NOTIFICATION_POLICY_REMOVE)
        }
        // 同一个存储目录只有一个引擎。将额外许可交回系统重排，避免关闭正在使用的引擎。
        if (waitingForEngine) {
            jobFinished(params, true)
            return true
        }
        val request = try {
            restoreRequest(params.extras)
        } catch (error: Exception) {
            SampleDownloads.showError(error)
            null
        } ?: return false
        if (!hasPermission(params)) return false
        val run = Run(params, request)
        active = run
        SampleDownloads.jobRunning = true

        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onLost(network: Network) = recheck(run, network, lost = true)
            override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) = recheck(run, network)
        }
        try {
            connectivity.registerNetworkCallback(NetworkRequest.Builder()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).build(), callback)
            run.callback = callback
        } catch (error: RuntimeException) {
            SampleDownloads.showError(error)
            detach(run)
            forgetCustom(params.extras)
            return false
        }
        val selected = if (Build.VERSION.SDK_INT >= 28) params.network
            else connectivity.allNetworks.firstOrNull { meetsConstraint(run, it) }
        useNetwork(run, selected)
        return true
    }

    private fun meetsConstraint(run: Run, network: Network): Boolean {
        val caps = connectivity.getNetworkCapabilities(network) ?: return false
        return caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) &&
            (!run.params.extras.getBoolean("unmetered") || caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED))
    }

    private fun recheck(run: Run, network: Network, lost: Boolean = false) {
        SampleDownloads.main.execute {
            if (active === run && run.network == network && hasPermission(run.params) &&
                (lost || !meetsConstraint(run, network))) {
                finish(run, reschedule = true, closeEngine = true)
            }
        }
    }

    override fun onNetworkChanged(params: JobParameters) {
        if (Build.VERSION.SDK_INT < 34) return
        val run = active ?: return
        if (matches(run, params)) useNetwork(run, params.network)
    }

    private fun useNetwork(run: Run, network: Network?) {
        if (run.finishing || !hasPermission(run.params)) return
        if (network == null || !meetsConstraint(run, network)) {
            finish(run, reschedule = true, closeEngine = run.network != null)
            return
        }
        if (run.network == network) return
        val attempt = Any()
        run.attempt = attempt
        run.network = network
        // jobEngine 先等待旧引擎关闭和检查点写入，再使用系统分配的新 Network。
        SampleDownloads.jobEngine(this, network).thenComposeAsync({
            if (active === run && run.attempt === attempt && !run.finishing && hasPermission(run.params) &&
                meetsConstraint(run, network)) {
                SampleDownloads.bind(this, run.request, true)
            } else CompletableFuture<com.luofelix.fetchkit.DownloadSubscription>().apply {
                completeExceptionally(FetchKitException(Status.Closed))
            }
        }, SampleDownloads.main).thenCompose { it.awaitStopped() }
            .whenCompleteAsync({ _, error ->
                if (active === run && run.attempt === attempt && !run.finishing && hasPermission(run.params)) {
                    val lostNetwork = !meetsConstraint(run, network)
                    if (!lostNetwork) SampleDownloads.showError(error)
                    finish(run, reschedule = lostNetwork, closeEngine = lostNetwork)
                }
            }, SampleDownloads.main)
    }

    private fun finish(run: Run, reschedule: Boolean, closeEngine: Boolean) {
        if (run.finishing) return
        run.finishing = true
        val stopped = if (closeEngine) SampleDownloads.stop() else CompletableFuture.completedFuture(Unit)
        stopped.whenCompleteAsync({ _, error ->
            if (active === run && hasPermission(run.params)) {
                detach(run)
                SampleDownloads.showError(error)
                SampleDownloads.releasePermission()
                jobFinished(run.params, reschedule && error == null)
                if (!reschedule || error != null) forgetCustom(run.params.extras)
            }
        }, SampleDownloads.main)
    }

    private fun detach(run: Run) {
        run.callback?.let { connectivity.unregisterNetworkCallback(it) }
        run.callback = null
        active = null
        SampleDownloads.jobRunning = false
    }

    override fun onStopJob(params: JobParameters): Boolean {
        val reschedule = !isWithdrawing(params.jobId) && (Build.VERSION.SDK_INT < 31 ||
            (params.stopReason != JobParameters.STOP_REASON_USER &&
                params.stopReason != JobParameters.STOP_REASON_CANCELLED_BY_APP))
        val run = active
        if (run != null && matches(run, params)) {
            detach(run)
            SampleDownloads.stop()
        }
        if (!reschedule) forgetCustom(params.extras)
        return reschedule
    }

    override fun onDestroy() {
        active?.let {
            detach(it)
            SampleDownloads.stop()
        }
        super.onDestroy()
    }
    companion object {
        const val JOB_ID = CUSTOM_DOWNLOAD_JOB_ID
        private var customRequest: Pair<String, DownloadRequest>? = null
        private val withdrawing = mutableSetOf<Int>()

        @Synchronized
        private fun isWithdrawing(jobId: Int): Boolean = jobId in withdrawing

        @Synchronized
        private fun forgetCustom(extras: PersistableBundle) {
            if (customRequest?.first == extras.getString("permit")) customRequest = null
        }

        @Synchronized
        private fun restoreRequest(extras: PersistableBundle): DownloadRequest? {
            val version = extras.getString("version") ?: "v1"
            val digest = extras.getString("sha256") ?: ""
            val pin = extras.getBoolean("pin")
            val preset = downloadPresets.firstOrNull { it.id == extras.getString("preset") }
            if (preset != null) return preset.request(version, digest, pin)
                .copy(expectedLength = if (extras.containsKey("length")) extras.getLong("length") else null)
            // 旧任务仍可读取其公开地址。新自定义任务只在本进程保留请求。
            val legacyUrl = extras.getString("url")
            if (legacyUrl != null) return sampleRequest(legacyUrl, digest, version, pin)
                .copy(expectedLength = if (extras.containsKey("length")) extras.getLong("length") else null)
            return customRequest?.takeIf { it.first == extras.getString("permit") }?.second
        }

        /** 用户操作开始前撤销相应 job 的续跑资格，SDK 暂停或取消完成后再取消系统 job。 */
        internal fun <T> withdraw(
            context: Context, identities: Set<ResourceIdentity>, operation: () -> CompletableFuture<T>,
        ): CompletableFuture<T> {
            val scheduler = context.getSystemService(JobScheduler::class.java)
            val service = ComponentName(context, DownloadJobService::class.java)
            val jobs = synchronized(this) {
                scheduler.allPendingJobs.filter {
                    it.service == service &&
                        runCatching { restoreRequest(it.extras)?.identity }.getOrNull()?.let { id -> id in identities } == true
                }.also { matching -> withdrawing.addAll(matching.map { it.id }) }
            }
            fun cancelJobs() = synchronized(this) {
                jobs.forEach {
                    scheduler.cancel(it.id)
                    forgetCustom(it.extras)
                    withdrawing.remove(it.id)
                }
            }
            return try {
                operation().whenCompleteAsync({ _, _ -> cancelJobs() }, SampleDownloads.main)
            } catch (error: Throwable) {
                cancelJobs()
                throw error
            }
        }
        fun schedule(context: Context, url: String, digest: String, version: String, pin: Boolean, unmetered: Boolean): Boolean {
            // 示例只接受公开地址；带凭证请求由业务宿主提供。
            return schedule(context, SampleDownloads.request(url, digest, version, pin), unmetered)
        }
        @Synchronized
        internal fun schedule(context: Context, request: DownloadRequest, unmetered: Boolean): Boolean {
            val preset = downloadPresets.firstOrNull {
                request.addressReference == "sample-preset:${it.id}" && request.request.url == it.url
            }
            val address = URI(request.request.url)
            require(preset != null || (address.scheme in setOf("http", "https") && address.host != null &&
                address.rawUserInfo == null && address.rawQuery == null && address.rawFragment == null)) {
                "后台自定义地址不能包含凭证、查询参数或片段；带参数的公开资源请使用预设"
            }
            // The static application UA is reconstructed by sampleRequest, never stored in extras.
            require(request.provider == null && (request.request.headers.isEmpty() ||
                request.request.headers == mapOf("User-Agent" to sampleUserAgent)))
            val scheduler = context.getSystemService(JobScheduler::class.java)
            val service = ComponentName(context, DownloadJobService::class.java)
            val scheduled = scheduler.allPendingJobs
            if (preset != null && scheduled.any {
                    it.service == service && it.extras.getString("preset") == preset.id &&
                        it.extras.getString("version") == request.identity.version
                }) return false
            val jobId = selectDownloadJobId(preset != null, scheduled.mapTo(withdrawing.toMutableSet()) { it.id })
                ?: return false
            val token = UUID.randomUUID().toString()
            val extras = PersistableBundle().apply {
                putString("permit", token)
                if (preset != null) putString("preset", preset.id)
                putString("sha256", request.expectedSha256); putString("version", request.identity.version)
                putBoolean("pin", request.pinOnCreate); putBoolean("unmetered", unmetered)
                request.expectedLength?.let { putLong("length", it) }
            }
            val job = JobInfo.Builder(jobId, service)
                .setRequiredNetworkType(if (unmetered) JobInfo.NETWORK_TYPE_UNMETERED else JobInfo.NETWORK_TYPE_ANY)
                .setExtras(extras)
            if (Build.VERSION.SDK_INT >= 34) job.setUserInitiated(true)
            if (preset == null) customRequest = token to request
            var accepted = false
            try {
                accepted = scheduler.schedule(job.build()) == JobScheduler.RESULT_SUCCESS
                return accepted
            } finally {
                if (!accepted) forgetCustom(extras)
            }
        }
    }
}
