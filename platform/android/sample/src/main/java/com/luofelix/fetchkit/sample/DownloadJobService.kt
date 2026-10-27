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
import android.net.NetworkCapabilities

/** Finite, interruptible regular jobs below API 34; user-initiated jobs on API 34+. */
class DownloadJobService : JobService() {
    private var active: JobParameters? = null
    override fun onStartJob(params: JobParameters): Boolean {
        active = params
        SampleDownloads.jobRunning = true
        if (Build.VERSION.SDK_INT >= 34 && params.isUserInitiatedJob) {
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(NotificationChannel("downloads", "文件下载", NotificationManager.IMPORTANCE_LOW))
            val notification = Notification.Builder(this, "downloads").setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle("FetchKit 正在下载").setContentText("可在应用内暂停或取消").setOngoing(true).build()
            setNotification(params, 4101, notification, JOB_END_NOTIFICATION_POLICY_REMOVE)
        }
        val request = SampleDownloads.request(params.extras.getString("url") ?: "",
            params.extras.getString("sha256") ?: "", params.extras.getString("version") ?: "v1", params.extras.getBoolean("pin"))
        val manager = getSystemService(ConnectivityManager::class.java)
        val selected = (if (Build.VERSION.SDK_INT >= 28) params.network else null) ?: manager.allNetworks.firstOrNull { network ->
            val caps = manager.getNetworkCapabilities(network)
            caps != null && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) &&
                (!params.extras.getBoolean("unmetered") || caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED))
        }
        if (selected == null) {
            SampleDownloads.main.execute {
                active = null; SampleDownloads.jobRunning = false; jobFinished(params, true)
            }
            return true
        }
        SampleDownloads.jobEngine(this, selected).thenCompose { it.enqueue(request) }.thenCompose { it.awaitStopped() }
            .whenCompleteAsync({ _, error ->
                if (active === params) {
                    active = null
                    SampleDownloads.jobRunning = false
                    SampleDownloads.showError(error)
                    SampleDownloads.releasePermission()
                    jobFinished(params, false)
                }
            }, SampleDownloads.main)
        return true
    }
    override fun onStopJob(params: JobParameters): Boolean {
        active = null
        SampleDownloads.jobRunning = false
        SampleDownloads.stop()
        return Build.VERSION.SDK_INT < 31 || params.stopReason != JobParameters.STOP_REASON_USER
    }
    companion object {
        const val JOB_ID = 4100
        fun schedule(context: Context, url: String, digest: String, version: String, pin: Boolean, unmetered: Boolean): Boolean {
            // Persist only public addresses; signed/private URLs need a host provider.
            val address = URI(url)
            require(address.scheme in setOf("http", "https") && address.host != null &&
                address.rawUserInfo == null && address.rawQuery == null && address.rawFragment == null)
            val extras = PersistableBundle().apply {
                putString("url", url); putString("sha256", digest); putString("version", version); putBoolean("pin", pin); putBoolean("unmetered", unmetered)
            }
            val job = JobInfo.Builder(JOB_ID, ComponentName(context, DownloadJobService::class.java))
                .setRequiredNetworkType(if (unmetered) JobInfo.NETWORK_TYPE_UNMETERED else JobInfo.NETWORK_TYPE_ANY)
                .setExtras(extras)
            if (Build.VERSION.SDK_INT >= 34) job.setUserInitiated(true)
            return context.getSystemService(JobScheduler::class.java).schedule(job.build()) == JobScheduler.RESULT_SUCCESS
        }
    }
}
