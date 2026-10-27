package com.luofelix.fetchkit.sample

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.luofelix.fetchkit.*
import com.luofelix.fetchkit.sample.ui.theme.FetchKitAndroidTheme

class MainActivity : ComponentActivity() {
    private val handler = Handler(Looper.getMainLooper())
    private val refresh = object : Runnable {
        override fun run() {
            if (!SampleDownloads.visible) return
            SampleDownloads.refresh(this@MainActivity).whenComplete { _, error ->
                SampleDownloads.showError(error)
                if (SampleDownloads.visible) handler.postDelayed(this, 500)
            }
        }
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent { FetchKitAndroidTheme {
            Scaffold(Modifier.fillMaxSize()) { padding -> DownloadScreen(Modifier.padding(padding)) }
        } }
    }
    override fun onStart() {
        super.onStart(); SampleDownloads.visible = true; handler.post(refresh)
    }
    override fun onStop() {
        SampleDownloads.visible = false; handler.removeCallbacks(refresh)
        if (!isChangingConfigurations) SampleDownloads.releasePermission()
        super.onStop()
    }
}

@Composable
private fun DownloadScreen(modifier: Modifier) {
    val context = LocalContext.current
    var url by remember { mutableStateOf("") }
    var digest by remember { mutableStateOf("") }
    var version by remember { mutableStateOf("v1") }
    var pinned by remember { mutableStateOf(false) }
    var unmetered by remember { mutableStateOf(true) }
    fun request() = SampleDownloads.request(url.trim(), digest.trim(), version.trim(), pinned)
    LazyColumn(modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        item {
            Text("FetchKit", style = MaterialTheme.typography.headlineMedium)
            Text("下载、恢复与资源读取 · ${FetchKit.version()}")
            OutlinedTextField(url, { url = it }, Modifier.fillMaxWidth(), label = { Text("资源 URL") }, singleLine = true)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(version, { version = it }, Modifier.weight(1f), label = { Text("业务版本") }, singleLine = true)
                Row { Checkbox(pinned, { pinned = it }); Text("长期保留", Modifier.padding(top = 16.dp)) }
            }
            OutlinedTextField(digest, { digest = it }, Modifier.fillMaxWidth(), label = { Text("可信 SHA-256（可选）") }, singleLine = true)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(enabled = url.isNotBlank() && version.isNotBlank(), onClick = {
                    runCatching { SampleDownloads.bind(context, request(), true).whenComplete { _, e -> SampleDownloads.showError(e) } }
                        .onFailure(SampleDownloads::showError)
                }) { Text("开始下载") }
                OutlinedButton(enabled = url.isNotBlank() && version.isNotBlank(), onClick = {
                    runCatching {
                        val scheduled = DownloadJobService.schedule(context, url.trim(), digest.trim(), version.trim(), pinned, unmetered)
                        SampleDownloads.message = if (scheduled) "已交给系统调度" else "系统未接受任务"
                    }.onFailure { SampleDownloads.message = "后台演示仅保存无凭证、无查询参数的公开地址" }
                }) { Text("后台排队") }
            }
            Row { Checkbox(unmetered, { unmetered = it }); Text("后台仅使用非计费网络", Modifier.padding(top = 16.dp)) }
            Text("离开页面后前台任务暂停；后台任务受系统执行许可约束。进程重开后重新填入地址和可信摘要，再恢复对应任务。",
                style = MaterialTheme.typography.bodySmall)
            if (SampleDownloads.message.isNotEmpty()) Text(SampleDownloads.message, color = MaterialTheme.colorScheme.primary)
        }
        items(SampleDownloads.rows, key = { it.snapshot.persistentId }) { row ->
            val s = row.snapshot
            val total = s.totalBytes
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text("${row.identity.version} · ${s.persistentId.take(12)}")
                    Text("${s.state} · ${s.receivedBytes} / ${s.totalBytes ?: "未知"} 字节")
                    if (total != null && total > 0) LinearProgressIndicator(
                        progress = { (s.receivedBytes.toFloat() / total).coerceIn(0f, 1f) }, modifier = Modifier.fillMaxWidth())
                    else if (!s.state.stopped) LinearProgressIndicator(Modifier.fillMaxWidth())
                    Text("检查点 ${s.committedOffset} · ${s.integrity} · ${s.error}", style = MaterialTheme.typography.bodySmall)
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        TextButton(enabled = url.isNotBlank(), onClick = {
                            SampleDownloads.bind(context, request().copy(identity = row.identity), false)
                                .thenCompose { it.pause() }.whenComplete { _, e -> SampleDownloads.showError(e) }
                        }) { Text("暂停") }
                        TextButton(enabled = url.isNotBlank(), onClick = {
                            SampleDownloads.bind(context, request().copy(identity = row.identity), true)
                                .whenComplete { _, e -> SampleDownloads.showError(e) }
                        }) { Text("恢复") }
                        TextButton(enabled = url.isNotBlank(), onClick = {
                            SampleDownloads.bind(context, request().copy(identity = row.identity), false)
                                .thenCompose { it.cancel() }.whenComplete { _, e -> SampleDownloads.showError(e) }
                        }) { Text("取消订阅") }
                    }
                    Row {
                        TextButton(onClick = { SampleDownloads.engine(context).thenCompose { it.pin(s.persistentId, !s.pinned) }
                            .whenComplete { _, e -> SampleDownloads.showError(e) } }) { Text(if (s.pinned) "取消保留" else "保留") }
                        TextButton(enabled = s.available, onClick = {
                            SampleDownloads.engine(context).thenCompose { it.acquire(s.persistentId) }.thenCompose { lease ->
                                lease.read(0, 64).whenComplete { _, _ -> lease.releaseAsync() }
                            }.whenCompleteAsync({ bytes, e ->
                                if (e == null) SampleDownloads.message = "租约读取 ${bytes.size} 字节" else SampleDownloads.showError(e)
                            }, SampleDownloads.main)
                        }) { Text("读取") }
                    }
                }
            }
        }
    }
}
