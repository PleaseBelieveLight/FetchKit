package com.luofelix.fetchkit.sample

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.luofelix.fetchkit.sample.ui.theme.FetchKitAndroidTheme

class MainActivity : ComponentActivity() {
    private val handler = Handler(Looper.getMainLooper())
    private var started = false
    private val refresh = object : Runnable {
        override fun run() {
            if (!started) return
            SampleDownloads.refresh(applicationContext).whenCompleteAsync({ _, error ->
                if (started) {
                    SampleDownloads.showError(error)
                    handler.postDelayed(this, 750)
                }
            }, SampleDownloads.main)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent { FetchKitAndroidTheme { DownloadApp() } }
    }

    override fun onStart() {
        super.onStart()
        started = true
        SampleDownloads.visible = true
        handler.post(refresh)
    }

    override fun onStop() {
        started = false
        SampleDownloads.visible = false
        handler.removeCallbacks(refresh)
        if (!isChangingConfigurations) SampleDownloads.releasePermission()
        super.onStop()
    }
}
