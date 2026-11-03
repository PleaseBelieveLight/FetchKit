package com.luofelix.fetchkit.sample

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.luofelix.fetchkit.*
import com.luofelix.fetchkit.coroutines.awaitValue
import kotlinx.coroutines.CancellationException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

@Composable
internal fun DownloadApp() {
    val context = LocalContext.current
    val newDownloadLabel = "新建下载"
    var page by rememberSaveable { mutableIntStateOf(0) }
    var adding by rememberSaveable { mutableStateOf(false) }
    var recoveryDemo by rememberSaveable { mutableStateOf(false) }
    var restoring by remember { mutableStateOf<StoredTask?>(null) }
    var querying by remember { mutableStateOf(false) }
    var exportId by rememberSaveable { mutableStateOf<String?>(null) }
    val exportDestination =
        rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
            val id = exportId
            exportId = null
            if (id != null && uri != null) SampleDownloads.export(context, id, uri)
        }
    val snackbar = remember { SnackbarHostState() }
    val message = SampleDownloads.message
    LaunchedEffect(message) {
        if (message.isNotBlank()) {
            snackbar.showSnackbar(message, duration = SnackbarDuration.Long)
            if (SampleDownloads.message == message) SampleDownloads.message = ""
        }
    }
    Scaffold(
        modifier = Modifier.testTag("sample-app"),
        topBar = { AppHeader() },
        bottomBar = {
            NavigationBar {
                listOf(
                    "下载" to "download",
                    "存储" to "storage",
                    "能力指南" to "guide"
                ).forEachIndexed { index, (label, icon) ->
                    NavigationBarItem(
                        selected = page == index, onClick = { page = index },
                        icon = { SampleIcon(icon) }, label = { Text(label) })
                }
            }
        },
        floatingActionButton = {
            // 显式设置名称，保证合并语义后辅助功能和外部 UI 回归仍能识别此入口。
            if (page == 0) ExtendedFloatingActionButton(
                onClick = { adding = true },
                modifier = Modifier.testTag("new-download").semantics { contentDescription = newDownloadLabel },
                icon = { SampleIcon("add") }, text = { Text(newDownloadLabel) })
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        when (page) {
            0 -> DownloadsPage(Modifier.padding(padding), { adding = true }, { querying = true }, { row ->
                SampleDownloads.queryTask(context, row).whenCompleteAsync({ latest, error ->
                    if (error != null) SampleDownloads.showError(error) else restoring = latest
                }, SampleDownloads.main)
            }, { row ->
                exportId = row.snapshot.persistentId
                val name = SampleDownloads.requestFor(row)?.request?.url?.substringBefore('?')?.substringAfterLast('/')
                    ?.takeIf { it.matches(Regex("[a-zA-Z0-9._-]{1,120}")) && it !in setOf(".", "..") }
                exportDestination.launch(name ?: "fetchkit-${row.snapshot.persistentId.take(12)}.bin")
            })

            1 -> StoragePage(Modifier.padding(padding))
            else -> GuidePage(Modifier.padding(padding)) { recoveryDemo = true; adding = true }
        }
    }
    if (adding || restoring != null) NewDownloadSheet(restoring, recoveryDemo) {
        adding = false; restoring = null; recoveryDemo = false
    }
    if (querying) TaskLookupDialog { querying = false }
}

@Composable
private fun AppHeader() {
    Row(
        Modifier.fillMaxWidth().statusBarsPadding().padding(horizontal = 20.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Surface(shape = RoundedCornerShape(14.dp), color = Color(0xFF153E43)) {
            Image(
                painter = painterResource(R.drawable.ic_shiliu_mark), contentDescription = null,
                modifier = Modifier.size(44.dp)
            )
        }
        Column(Modifier.weight(1f)) {
            Text(
                stringResource(R.string.app_name),
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.Bold
            )
            Text(
                stringResource(R.string.app_subtitle),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        Surface(shape = RoundedCornerShape(50), color = MaterialTheme.colorScheme.surfaceContainerHigh) {
            Text(
                "v${remember { FetchKit.version() }}", Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
                style = MaterialTheme.typography.labelMedium, fontFamily = FontFamily.Monospace
            )
        }
    }
}

@Composable
private fun DownloadsPage(
    modifier: Modifier, onAdd: () -> Unit, onLookup: () -> Unit,
    onRestore: (StoredTask) -> Unit, onExport: (StoredTask) -> Unit,
) {
    val context = LocalContext.current
    var filter by rememberSaveable { mutableIntStateOf(0) }
    val rows = SampleDownloads.rows
    val active = rows.count { !it.snapshot.state.stopped }
    val completed = rows.count { it.snapshot.state == TaskState.Completed }
    val filtered = rows.filter {
        when (filter) {
            1 -> !it.snapshot.state.stopped
            2 -> it.snapshot.state in setOf(TaskState.Paused, TaskState.Failed)
            3 -> it.snapshot.state == TaskState.Completed
            else -> true
        }
    }
    LazyColumn(
        modifier.fillMaxSize(), contentPadding = PaddingValues(start = 20.dp, end = 20.dp, top = 6.dp, bottom = 96.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        item {
            Column(
                Modifier.fillMaxWidth().background(
                    Brush.linearGradient(listOf(Color(0xFF182E42), Color(0xFF244F5A))),
                    RoundedCornerShape(24.dp)
                ).padding(22.dp), verticalArrangement = Arrangement.spacedBy(18.dp)
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(
                            "每一份下载，都有迹可循", color = Color.White, style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.SemiBold
                        )
                        Text(
                            "多文件并发 · 可靠续传 · 校验后发布", color = Color(0xFFB7CED3),
                            style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(top = 6.dp)
                        )
                    }
                }
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    HeroMetric("正在进行", active.toString())
                    HeroMetric("已完成", completed.toString())
                    HeroMetric("总下载速度", "${formatBytes(SampleDownloads.speeds.values.sum())}/s")
                }
            }
        }
        if (SampleDownloads.backgroundJobs.isNotEmpty()) item { BackgroundQueue() }
        item {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "下载任务",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.weight(1f)
                )
                TextButton(
                    onClick = { SampleDownloads.pauseAll(context) },
                    enabled = !SampleDownloads.busy && active > 0
                ) { Text("全部暂停") }
                TextButton(onClick = { SampleDownloads.resumeAll(context) }, enabled = !SampleDownloads.busy && rows.any {
                    it.snapshot.canResume && SampleDownloads.requestFor(it) != null
                }) { Text("全部恢复") }
            }
            TextButton(onClick = onLookup, contentPadding = PaddingValues(horizontal = 8.dp)) { Text("按 ID 查询") }
            @OptIn(ExperimentalLayoutApi::class)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(
                    "全部 ${rows.size}",
                    "进行中 $active",
                    "已暂停 / 失败",
                    "已完成 $completed"
                ).forEachIndexed { index, label ->
                    FilterChip(selected = filter == index, onClick = { filter = index }, label = { Text(label) })
                }
            }
        }
        if (filtered.isEmpty()) item {
            Panel {
                SampleIcon("download", Modifier.size(36.dp), MaterialTheme.colorScheme.primary)
                Text(
                    if (rows.isEmpty()) "从第一份文件开始" else "这里暂时没有任务",
                    style = MaterialTheme.typography.titleMedium
                )
                Hint(if (rows.isEmpty()) "可用中科大或南大镜像体验下载进度、暂停和续传，也可以添加自己的地址。" else "切换筛选查看其他下载。")
                if (rows.isEmpty()) Button(onClick = onAdd) { Text("选择示例资源") }
            }
        }
        items(filtered, key = { it.snapshot.persistentId }) { row -> TaskCard(row, onRestore, onExport) }
        if (rows.size >= SampleDownloads.displayLimit) item {
            TextButton(onClick = { SampleDownloads.loadMore(context) }) { Text("加载更多任务") }
        }
        item { Hint("离开应用后前台下载会保存检查点并暂停。预设任务可在重开后恢复；后台执行请在新建下载中选择系统调度。") }
    }
}

@Composable
private fun BackgroundQueue() {
    val jobs = SampleDownloads.backgroundJobs
    var expanded by rememberSaveable { mutableStateOf(false) }
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("后台队列", Modifier.weight(1f), style = MaterialTheme.typography.titleMedium)
            TextButton(onClick = { expanded = !expanded }) { Text(if (expanded) "收起" else "查看 ${jobs.size} 项") }
        }
        Hint("执行中 ${jobs.count { it.running }} · 等待系统调度 ${jobs.count { !it.running }}")
        if (expanded) jobs.forEach { job ->
            KeyValue(
                "${job.title} · #${job.id}",
                "${if (job.running) "执行中" else "等待调度"} · ${if (job.unmetered) "仅非计费" else "任意网络"}"
            )
        }
    }
}

@Composable
private fun TaskLookupDialog(row: StoredTask? = null, onDismiss: () -> Unit) {
    val context = LocalContext.current
    var id by remember { mutableStateOf(row?.snapshot?.persistentId.orEmpty()) }
    var byIdentity by remember { mutableStateOf(false) }
    var lookup by remember { mutableIntStateOf(if (row == null) 0 else 1) }
    var result by remember { mutableStateOf<StoredTask?>(null) }
    var error by remember { mutableStateOf("") }
    var loading by remember { mutableStateOf(false) }
    val validId = id.trim().matches(Regex("[0-9a-f]{64}"))
    LaunchedEffect(lookup, id, byIdentity) {
        result = null
        error = ""
        loading = false
        if (lookup == 0 || (!byIdentity && !validId)) return@LaunchedEffect
        loading = true
        try {
            result = if (row != null && byIdentity) SampleDownloads.queryTask(context, row.identity).awaitValue()
            else SampleDownloads.queryTask(context, id.trim()).awaitValue()
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: FetchKitException) {
            error = if (failure.status == Status.NotFound) "没有找到这个任务" else "查询失败 · ${failure.status}"
        } catch (_: Exception) {
            error = "暂时无法查询任务"
        } finally {
            loading = false
        }
    }
    AlertDialog(onDismissRequest = onDismiss, title = { Text("即时查询任务") }, text = {
        Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Hint("直接读取一条 SDK 快照，可查询列表尚未加载的任务。")
            if (row == null) {
                OutlinedTextField(
                    id,
                    { id = it.take(64); lookup = 0 },
                    Modifier.testTag("task-lookup-id"),
                    label = { Text("持久化 ID") },
                    supportingText = { Text("64 位小写十六进制字符") },
                    singleLine = true
                )
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FilterChip(!byIdentity, onClick = { byIdentity = false }, label = { Text("持久化 ID") })
                    FilterChip(byIdentity, onClick = { byIdentity = true }, label = { Text("资源身份") })
                }
            }
            if (loading) LinearProgressIndicator(Modifier.fillMaxWidth())
            if (error.isNotEmpty()) Text(error, color = MaterialTheme.colorScheme.error)
            result?.let { stored ->
                val s = stored.snapshot
                KeyValue("状态", s.state.label())
                KeyValue("进度 / 检查点", "${formatBytes(s.receivedBytes)} / ${formatBytes(s.committedOffset)}")
                KeyValue("运行代次 / 活跃订阅", "${s.generation} / ${s.subscriberCount}")
                KeyValue("完整性", integrityLabel(s.integrity))
                SelectionContainer {
                    Text(
                        s.persistentId,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace
                    )
                }
            }
        }
    }, confirmButton = {
        TextButton(enabled = !loading && (byIdentity || validId), onClick = { lookup++ }) { Text("查询") }
    }, dismissButton = { TextButton(onClick = onDismiss) { Text("关闭") } })
}

@Composable
private fun HeroMetric(label: String, value: String) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(value, color = Color.White, fontSize = 21.sp, fontWeight = FontWeight.SemiBold)
        Text(label, color = Color(0xFFB7CED3), style = MaterialTheme.typography.labelSmall)
    }
}

@Composable
private fun TaskCard(row: StoredTask, onRestore: (StoredTask) -> Unit, onExport: (StoredTask) -> Unit) {
    val context = LocalContext.current
    val s = row.snapshot
    val total = s.totalBytes
    var details by rememberSaveable(s.persistentId) { mutableStateOf(false) }
    var lookup by remember { mutableStateOf(false) }
    val known = SampleDownloads.requestFor(row) != null
    val speed = SampleDownloads.speeds[s.persistentId] ?: 0
    val enabled = !SampleDownloads.busy
    val fraction = s.totalBytes?.takeIf { it > 0 }?.let { (s.receivedBytes.toFloat() / it).coerceIn(0f, 1f) }
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Surface(color = MaterialTheme.colorScheme.primaryContainer, shape = RoundedCornerShape(14.dp)) {
                SampleIcon(if (s.state == TaskState.Completed) "check" else "file", Modifier.padding(12.dp))
            }
            Column(Modifier.weight(1f)) {
                Text(
                    SampleDownloads.title(row),
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold
                )
                Text(
                    "${row.identity.version} · ${s.state.label()}", style = MaterialTheme.typography.bodySmall,
                    color = if (s.state == TaskState.Failed) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.primary
                )
            }
            if (fraction != null) Text(
                "${(fraction * 100).toInt()}%",
                style = MaterialTheme.typography.titleMedium,
                fontFamily = FontFamily.Monospace
            )
        }
        if (fraction != null) LinearProgressIndicator(progress = { fraction }, modifier = Modifier.fillMaxWidth())
        else if (!s.state.stopped) LinearProgressIndicator(Modifier.fillMaxWidth())
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(
                "${formatBytes(s.receivedBytes)} / ${s.totalBytes?.let(::formatBytes) ?: "大小未知"}",
                style = MaterialTheme.typography.bodySmall
            )
            Text(
                if (s.state == TaskState.Running) "${formatBytes(speed)}/s" else if (s.pinned) "已保留" else "",
                style = MaterialTheme.typography.bodySmall
            )
        }
        if (speed > 0 && total != null && total > s.receivedBytes) Hint("预计剩余 ${(total - s.receivedBytes) / speed} 秒")
        if (s.state == TaskState.Failed) Text(
            "${s.error} · HTTP ${s.httpStatus}",
            color = MaterialTheme.colorScheme.error,
            style = MaterialTheme.typography.bodySmall
        )
        if (s.state == TaskState.RetryWaiting) Hint("第 ${s.retryCount} 次自动重试 · 下次 ${formatTime(s.nextRetryAtMs)}")
        @OptIn(ExperimentalLayoutApi::class)
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            if (!s.state.stopped) OutlinedButton(
                enabled = enabled && known && s.state != TaskState.Pausing,
                onClick = { SampleDownloads.control(context, row, "pause") }) {
                SampleIcon(
                    "pause",
                    Modifier.size(16.dp)
                ); Spacer(Modifier.width(4.dp)); Text("暂停")
            }
            if (s.canRetryWithRequest) {
                Button(enabled = enabled, onClick = {
                    if (known && s.state == TaskState.Paused) SampleDownloads.control(context, row, "resume") else onRestore(
                        row
                    )
                }) {
                    SampleIcon(
                        "play",
                        Modifier.size(16.dp)
                    ); Spacer(Modifier.width(4.dp)); Text(if (s.state == TaskState.Failed) "更新地址并重试" else if (known) "恢复下载" else "重新绑定")
                }
            }
            if (s.available) OutlinedButton(
                enabled = enabled,
                onClick = { SampleDownloads.read(context, row) }) { Text("租约读取") }
            if (s.available && SampleDownloads.exportingId == null)
                OutlinedButton(enabled = enabled, onClick = { onExport(row) }) { Text("导出文件") }
            if (SampleDownloads.exportingId == s.persistentId) {
                OutlinedButton(onClick = SampleDownloads::cancelExport) { Text("取消导出") }
            }
            TextButton(onClick = { details = !details }) { Text(if (details) "收起详情" else "任务详情") }
        }
        if (details) {
            HorizontalDivider()
            val observation = SampleDownloads.snapshotsFor(row)
            val live = if (observation != null) observation.collectAsState().value else null
            if (live != null) KeyValue("实时快照", "${live.state.label()} · ${formatBytes(live.receivedBytes)}")
            TextButton(onClick = { lookup = true }) { Text("即时查询任务") }
            KeyValue("已提交检查点", formatBytes(s.committedOffset))
            KeyValue("运行代次 / 请求数", "${s.generation} / ${s.requestCount}")
            KeyValue("HTTP / 平台码", "${s.httpStatus} / ${s.platformCode}")
            KeyValue("自动重试 / 活跃订阅", "${s.retryCount} / ${s.subscriberCount}")
            KeyValue("完整性", integrityLabel(s.integrity))
            KeyValue("可读取 / 过期时间", "${if (s.available) "是" else "否"} / ${formatTime(s.expiresAtMs)}")
            Hint("暂停确认后检查点才持久化；续传时校验 Range 和资源版本，不支持时安全重下。")
            if (s.state == TaskState.Failed && !s.canRetryWithRequest) Hint("该错误不可直接重试。请到存储页移除资源，检查地址及校验参数后重新创建。")
            @OptIn(ExperimentalLayoutApi::class)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                TextButton(
                    enabled = enabled && known && SampleDownloads.subscriptionCount(row) < 4,
                    onClick = { SampleDownloads.control(context, row, "subscribe") }) { Text("添加订阅") }
                TextButton(
                    enabled = enabled && SampleDownloads.subscriptionCount(row) > 0,
                    onClick = { SampleDownloads.control(context, row, "cancel") }) { Text("取消一个订阅") }
                TextButton(
                    enabled = enabled && SampleDownloads.subscriptionCount(row) > 0,
                    onClick = { SampleDownloads.control(context, row, "release") }) { Text("释放一个订阅") }
            }
            if (s.state == TaskState.Paused) TextButton(enabled = enabled, onClick = { onRestore(row) }) {
                Text("更新地址并恢复")
            }
            Hint("取消只撤销一个订阅；最后一个取消会清理未完成数据。释放最后一个订阅则暂停并保留检查点。")
        }
    }
    if (lookup) TaskLookupDialog(row) { lookup = false }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
private fun NewDownloadSheet(restoring: StoredTask?, recoveryDemo: Boolean, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val original = restoring?.let { SampleDownloads.requestFor(it) }
    var selected by remember {
        mutableStateOf(setOf(downloadPresets.firstOrNull {
            resourceKey(it.url) == restoring?.identity?.key
        }?.id ?: downloadPresets.first().id))
    }
    var custom by remember { mutableStateOf(restoring != null || recoveryDemo) }
    var url by remember {
        mutableStateOf(
            when {
                restoring?.identity?.key == recoveryDemoKey -> recoveryDemoFile.url
                recoveryDemo -> recoveryDemoAddress
                else -> original?.request?.url.orEmpty()
            }
        )
    }
    var version by rememberSaveable {
        mutableStateOf(
            restoring?.identity?.version ?: if (recoveryDemo) "demo-${System.currentTimeMillis()}" else "v1"
        )
    }
    var digest by rememberSaveable { mutableStateOf(original?.expectedSha256 ?: "") }
    var length by rememberSaveable { mutableStateOf(original?.expectedLength?.toString() ?: "") }
    var pinned by rememberSaveable { mutableStateOf(false) }
    var advanced by rememberSaveable { mutableStateOf(restoring != null) }
    var background by rememberSaveable { mutableStateOf(false) }
    var unmetered by rememberSaveable { mutableStateOf(true) }
    var morePresets by rememberSaveable { mutableStateOf(false) }
    var refreshUrl by remember { mutableStateOf("") }
    var refreshMissing by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }
    val count = if (custom) 1 else selected.size
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        modifier = Modifier.windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top)).padding(top = 24.dp),
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
    ) {
        Column(
            Modifier.fillMaxWidth().fillMaxHeight().imePadding().testTag("download-form")
                .verticalScroll(rememberScrollState())
                .padding(start = 24.dp, end = 24.dp, bottom = 28.dp), verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text(
                if (restoring == null) "新建下载" else "换链恢复",
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.Bold
            )
            if (restoring == null) Hint(
                if (recoveryDemo) "此地址用于模拟链接失效。提交后返回下载页，等待失败，再点「更新地址并重试」。"
                else "选择一个或多个测试文件，立即体验 SDK。"
            ) else {
                KeyValue("资源", SampleDownloads.title(restoring))
                KeyValue("已保存检查点", formatBytes(restoring.snapshot.committedOffset))
                KeyValue(
                    "上次结果", "${restoring.snapshot.state.label()} · ${restoring.snapshot.error}" +
                            restoring.snapshot.httpStatus.takeIf { it > 0 }?.let { " · HTTP $it" }.orEmpty()
                )
                Hint("输入同一资源的新地址，保留原长度和摘要约束。SDK 会校验检查点后决定是否续传。")
            }
            if (!custom) (if (morePresets) downloadPresets else mainDownloadPresets).forEach { preset ->
                if (preset == largeDownloadPresets.first()) Hint("大文件续传")
                if (preset == quickDownloadPresets.first()) Hint("快捷验证 · 查询与导出")
                val checked = preset.id in selected
                Surface(
                    shape = RoundedCornerShape(16.dp),
                    color = if (checked) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceContainer,
                    modifier = Modifier.fillMaxWidth().testTag("preset-${preset.id}")
                        .toggleable(checked, role = Role.Checkbox, onValueChange = {
                            selected =
                                if (restoring != null) setOf(preset.id) else if (checked) selected - preset.id else selected + preset.id
                            if (restoring == null) {
                                digest = ""; length = ""
                            }
                        })
                ) {
                    Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked, null)
                        Column(Modifier.weight(1f).padding(start = 10.dp)) {
                            Text(preset.title, style = MaterialTheme.typography.titleSmall)
                            Text(
                                "${preset.size} · ${preset.url.substringAfter("https://").substringBefore('/')}",
                                style = MaterialTheme.typography.bodySmall,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }
                }
            }
            if (!custom) TextButton(onClick = { morePresets = !morePresets }, contentPadding = PaddingValues(0.dp)) {
                Text(if (morePresets) "收起更多镜像" else "更多镜像 · 大文件与快捷验证")
            }
            if (restoring == null) Row(verticalAlignment = Alignment.CenterVertically) {
                Text("使用自定义地址（可选）", Modifier.weight(1f))
                Switch(custom, {
                    custom = it; if (restoring == null) {
                    digest = ""; length = ""
                }
                })
            }
            if (custom) OutlinedTextField(
                url, { url = it }, Modifier.fillMaxWidth(), label = { Text("HTTP(S) 下载地址") },
                singleLine = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                supportingText = { Text("仅保留在当前进程；重开后需重新提供") })
            TextButton(
                onClick = { advanced = !advanced },
                contentPadding = PaddingValues(0.dp)
            ) { Text(if (advanced) "收起高级选项" else "高级选项 · 校验与资源策略") }
            if (advanced) {
                OutlinedTextField(
                    version,
                    { version = it },
                    Modifier.fillMaxWidth(),
                    label = { Text("业务版本") },
                    singleLine = true,
                    enabled = restoring == null
                )
                if (count == 1) {
                    OutlinedTextField(
                        digest,
                        { digest = it },
                        Modifier.fillMaxWidth(),
                        label = { Text("可信 SHA-256（可选）") },
                        singleLine = true,
                        readOnly = original != null,
                        supportingText = { Text("填写后必须通过摘要校验才会发布；测试地址未提供可信摘要") })
                    OutlinedTextField(
                        length,
                        { length = it },
                        Modifier.fillMaxWidth(),
                        label = { Text("预期字节数（可选）") },
                        singleLine = true,
                        readOnly = original != null,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
                    )
                    if (!background) {
                        OutlinedTextField(
                            refreshUrl, { refreshUrl = it }, Modifier.fillMaxWidth(),
                            label = { Text("鉴权刷新地址（可选）") }, singleLine = true,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                            supportingText = { Text("401 / 403 时由示例 Provider 提供此地址，仅保留在当前进程") })
                        if (refreshUrl.isNotBlank()) ToggleRow(
                            "404 / 410 也尝试刷新", "用于把链接过期报告为资源不存在的服务端",
                            refreshMissing, { refreshMissing = it })
                    }
                } else Hint("批量资源使用各自响应长度；如需可信 SHA-256，请逐个新建并填写。")
                if (restoring == null) ToggleRow("长期保留", "从创建起阻止 SDK 的 LRU 淘汰", pinned, { pinned = it })
            }
            HorizontalDivider()
            if (restoring == null) ToggleRow(
                "交给系统后台调度",
                "API 34+ 使用用户发起的数据传输任务",
                background,
                { background = it },
                count == 1
            )
            if (background && count == 1) ToggleRow(
                "仅非计费网络",
                "等待满足网络条件后由系统决定执行时间",
                unmetered,
                { unmetered = it })
            if (background && count == 1) Hint("后台任务独立排队，每次执行一个。前台下载会保存检查点并暂停。")
            if (restoring == null) Hint(
                if (background && count == 1) "后台支持公开预设，或无查询参数的公开自定义地址。预设地址按资源 ID 重建。"
                else "最多同时下载 3 个文件，网络错误自动重试最多 3 次。下载站点的可用性与续传能力由服务端决定。"
            )
            if (error.isNotEmpty()) Text(
                error,
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodySmall
            )
            Button(
                enabled = !SampleDownloads.busy && count > 0 && (!custom || url.isNotBlank()),
                modifier = Modifier.fillMaxWidth().testTag("submit-download"),
                contentPadding = PaddingValues(16.dp),
                onClick = {
                    runCatching {
                        require(length.isBlank() || (length.toLongOrNull() ?: -1) >= 0) { "预期长度必须是非负整数" }
                        val hash = if (count == 1) digest else ""
                        val requests = (if (custom) listOf(
                            sampleRequest(
                                url,
                                hash,
                                version,
                                original?.pinOnCreate ?: restoring?.snapshot?.pinned ?: pinned
                            )
                        )
                        else downloadPresets.filter { it.id in selected }.map { it.request(version, hash, pinned) })
                            .map { request -> request.copy(expectedLength = if (count == 1) length.toLongOrNull() else null) }
                            .map { request ->
                                if (restoring == null) request else request.copy(
                                    identity = restoring.identity,
                                    addressReference = restoring.addressReference
                                )
                            }
                            .map { request ->
                                if (count == 1 && !background && refreshUrl.isNotBlank())
                                    withSampleRefresh(request, refreshUrl, refreshMissing) else request
                            }
                        if (background && count == 1 && restoring == null) {
                            require(
                                DownloadJobService.schedule(
                                    context,
                                    requests.single(),
                                    unmetered
                                )
                            ) { "系统未接受任务，请稍后重试" }
                            SampleDownloads.message = "已交给系统调度，可在系统授予执行机会后查看任务"
                        } else if (restoring != null) SampleDownloads.recover(context, requests.single(), onDismiss)
                        else SampleDownloads.start(context, requests)
                        if (restoring == null) onDismiss()
                    }.onFailure { error = it.message ?: "请检查输入" }
                }) {
                SampleIcon(if (background && count == 1) "storage" else "download", Modifier.size(18.dp))
                Spacer(Modifier.width(8.dp))
                Text(if (background && count == 1) "后台排队" else if (restoring != null) "使用新地址重试" else "开始下载 · $count 个文件")
            }
            Hint("文件较大，建议使用 Wi-Fi。实际文件大小以服务端响应为准。")
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun StoragePage(modifier: Modifier) {
    val context = LocalContext.current
    var removing by remember { mutableStateOf<StoredTask?>(null) }
    var trimming by remember { mutableStateOf(false) }
    val stats = SampleDownloads.stats
    val enabled = !SampleDownloads.busy
    LazyColumn(
        modifier.fillMaxSize(),
        contentPadding = PaddingValues(20.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        item {
            Text("资源与存储", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
            Hint("观察配额与资源保护，读取只对已发布且未过期的对象开放。")
        }
        item {
            Panel {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("SDK 配额", style = MaterialTheme.typography.titleMedium)
                    Text(
                        "${formatBytes(stats?.chargedBytes ?: 0)} / ${formatBytes(stats?.capacityBytes ?: (4L shl 30))}",
                        style = MaterialTheme.typography.labelLarge
                    )
                }
                LinearProgressIndicator(progress = {
                    if (stats == null) 0f else (stats.chargedBytes.toFloat() / stats.capacityBytes).coerceIn(
                        0f,
                        1f
                    )
                }, modifier = Modifier.fillMaxWidth())
                KeyValue(
                    "已使用 / 已预留",
                    "${formatBytes(stats?.usedBytes ?: 0)} / ${formatBytes(stats?.reservedBytes ?: 0)}"
                )
                KeyValue(
                    "元数据预算 / 记录数",
                    "${formatBytes(stats?.metadataBudgetBytes ?: 0)} / ${stats?.recordCount ?: 0}"
                )
                KeyValue(
                    "隔离 / 未知版本 / 孤立文件",
                    "${stats?.quarantinedRecords ?: 0} / ${stats?.unsupportedRecords ?: 0} / ${stats?.orphanFiles ?: 0}"
                )
                Hint("配额包含暂存、成品和元数据。它不是设备的剩余磁盘空间。未知文件保留并计费。")
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(enabled = enabled, onClick = { trimming = true }) { Text("LRU 回收至 512 MiB") }
                    TextButton(enabled = enabled, onClick = { SampleDownloads.collect(context) }) { Text("清理过期暂存") }
                }
            }
        }
        if (SampleDownloads.leasedId != null) item {
            Panel {
                Text(
                    "读取租约已持有",
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.primary
                )
                Hint("该资源受租约保护，删除和淘汰会被阻止。下面是文件前 64 字节。")
                Text(
                    SampleDownloads.readPreview,
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace
                )
                OutlinedButton(
                    enabled = enabled,
                    onClick = { SampleDownloads.releaseRead(context) }) { Text("释放读取租约") }
            }
        }
        item { Text("资源策略", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold) }
        if (SampleDownloads.rows.isEmpty()) item { Panel { Hint("下载文件后，可在此演示保留、过期、租约读取和安全删除。") } }
        items(SampleDownloads.rows, key = { it.snapshot.persistentId }) { row ->
            val s = row.snapshot
            var expiryMenu by remember { mutableStateOf(false) }
            Panel {
                Text(
                    SampleDownloads.title(row),
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold
                )
                Hint("${s.state.label()} · ${formatBytes(s.receivedBytes)} · ${integrityLabel(s.integrity)}")
                KeyValue("保留 / 过期", "${if (s.pinned) "长期保留" else "可回收"} / ${formatTime(s.expiresAtMs)}")
                if (SampleDownloads.leasedId == s.persistentId) Hint("当前持有读取租约")
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    TextButton(
                        enabled = enabled,
                        onClick = { SampleDownloads.pin(context, row) }) { Text(if (s.pinned) "取消保留" else "长期保留") }
                    TextButton(
                        enabled = enabled && s.available,
                        onClick = { SampleDownloads.read(context, row) }) { Text("租约读取") }
                    Box {
                        TextButton(enabled = enabled, onClick = { expiryMenu = true }) { Text("设置过期") }
                        DropdownMenu(expiryMenu, { expiryMenu = false }) {
                            listOf(
                                "10 分钟后过期" to System.currentTimeMillis() + 600000,
                                "立即过期" to System.currentTimeMillis(),
                                "清除过期时间" to 0L
                            ).forEach { (label, time) ->
                                DropdownMenuItem(
                                    text = { Text(label) },
                                    onClick = { expiryMenu = false; SampleDownloads.expiry(context, row, time) })
                            }
                        }
                    }
                    TextButton(
                        enabled = enabled && s.state.stopped && !s.pinned && SampleDownloads.leasedId != s.persistentId,
                        onClick = { removing = row }) {
                        Text(
                            "删除",
                            color = if (enabled && s.state.stopped && !s.pinned && SampleDownloads.leasedId != s.persistentId)
                                MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }
        }
        if (SampleDownloads.rows.size >= SampleDownloads.displayLimit) item {
            TextButton(onClick = { SampleDownloads.loadMore(context) }) { Text("加载更多资源") }
        }
    }
    if (removing != null) AlertDialog(
        onDismissRequest = { removing = null }, title = { Text("删除资源？") },
        text = { Text("将移除此资源的数据和记录，再次使用需要重新下载。") },
        confirmButton = {
            TextButton(onClick = {
                removing?.let { SampleDownloads.remove(context, it) }; removing = null
            }) { Text("删除") }
        },
        dismissButton = { TextButton(onClick = { removing = null }) { Text("保留") } })
    if (trimming) AlertDialog(
        onDismissRequest = { trimming = false }, title = { Text("按 LRU 回收缓存？") },
        text = { Text("尝试将计费空间降至 512 MiB，优先删除最久未使用的已完成资源。保留、租约和活动任务受保护；空间不足时可能只完成部分回收。") },
        confirmButton = { TextButton(onClick = { trimming = false; SampleDownloads.trim(context) }) { Text("开始回收") } },
        dismissButton = { TextButton(onClick = { trimming = false }) { Text("取消") } })
}

@Composable
private fun GuidePage(modifier: Modifier, onRecoveryDemo: () -> Unit) {
    LazyColumn(
        modifier.fillMaxSize(),
        contentPadding = PaddingValues(20.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        item {
            Text("把能力跑一遍", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
            Hint("所有操作调用真实 SDK。按下面的路径观察任务、记录与资源的变化。")
        }
        item {
            Panel {
                Text("链接失效后恢复", style = MaterialTheme.typography.titleMedium)
                Hint("用清华镜像的缺失地址触发 HTTP 404，再以同一资源身份换到可用的校验文件。")
                OutlinedButton(onClick = onRecoveryDemo) { Text("准备换链演练") }
            }
        }
        items(
            listOf(
                Triple("01", "多文件并发", "新建下载 → 选中多个预设 → 开始下载。观察各文件独立进度，最多 3 个同时运行。"),
                Triple(
                    "02",
                    "暂停与断点续传",
                    "下载中暂停 → 等待检查点保存 → 恢复。任务详情展示收到的字节、持久化偏移和运行代次。服务端不支持续传或资源变更时安全重下。"
                ),
                Triple(
                    "03",
                    "重开应用后恢复",
                    "下载中离开应用，再打开并点击恢复。预设配置仅保存资源 ID 和非敏感约束；自定义地址需重新绑定，SDK 不保存 URL 和凭证。"
                ),
                Triple(
                    "04",
                    "独立订阅与取消",
                    "任务详情 → 添加订阅 → 取消一个订阅。另一个继续；最后一个显式取消才清理未完成数据。释放订阅则保留检查点。"
                ),
                Triple(
                    "05",
                    "完整性与安全发布",
                    "高级选项可填写可信 SHA-256 和预期长度。普通传输完成与摘要已验证分别显示；填写不匹配摘要可观察失败且不可读取。"
                ),
                Triple(
                    "06",
                    "租约与缓存策略",
                    "存储 → 租约读取 → 观察删除受保护 → 释放租约。可切换长期保留、设置过期、按 LRU 回收以及清理 7 天前暂存。"
                ),
                Triple(
                    "07",
                    "自动重试与后台调度",
                    "网络错误或可重试 HTTP 状态最多重试 3 次，详情显示次数和截止时间。新建单文件可交给 JobScheduler；系统授予机会后执行。"
                ),
            )
        ) { (number, title, text) ->
            Panel {
                Text(
                    "$number  /  $title",
                    color = MaterialTheme.colorScheme.primary,
                    style = MaterialTheme.typography.titleMedium
                )
                Hint(text)
            }
        }
        item {
            Panel {
                Text("当前引擎配置", style = MaterialTheme.typography.titleMedium)
                KeyValue("并发 / 排队上限", "3 / 1024")
                KeyValue("单文件上限 / 存储配额", "2 GiB / 4 GiB")
                KeyValue("检查点阈值", "4 MiB 或 1 秒")
                KeyValue("重试 / 暂存保留", "最多 3 次 / 7 天")
                Hint("请求头、鉴权刷新 RequestProvider、网络绑定和超时配置等高级接入见 docs/reference/AndroidSDK接口与行为.md。凭证由宿主即时提供，避免写入任务记录。")
            }
        }
        item { Text("最近状态快照", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold) }
        item {
            Panel {
                if (SampleDownloads.events.isEmpty()) Hint("开始下载后，在此显示订阅观察到的状态变化，最多保留 60 条。")
                else SampleDownloads.events.forEach {
                    Text(
                        it,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace
                    )
                }
            }
        }
    }
}

@Composable
private fun Panel(content: @Composable ColumnScope.() -> Unit) {
    Surface(
        Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        color = MaterialTheme.colorScheme.surfaceContainerLow
    ) {
        Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(10.dp), content = content)
    }
}

@Composable
private fun Hint(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        lineHeight = 20.sp
    )
}

@Composable
private fun KeyValue(label: String, value: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(
            label,
            Modifier.weight(1f),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(value, Modifier.weight(1f), style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
    }
}

@Composable
private fun ToggleRow(
    title: String,
    subtitle: String,
    checked: Boolean,
    onChange: (Boolean) -> Unit,
    enabled: Boolean = true
) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Column(Modifier.weight(1f)) { Text(title, style = MaterialTheme.typography.bodyMedium); Hint(subtitle) }
        Switch(checked, onChange, enabled = enabled)
    }
}

private fun integrityLabel(integrity: Integrity): String = when (integrity) {
    Integrity.None -> "尚未校验发布"
    Integrity.TransportComplete -> "传输完整 · 未验证可信摘要"
    Integrity.DigestVerified -> "SHA-256 已验证"
}

private fun formatTime(time: Long): String =
    if (time == 0L) "未设置" else SimpleDateFormat("MM-dd HH:mm:ss", Locale.ROOT).format(Date(time))

@Composable
private fun SampleIcon(name: String, modifier: Modifier = Modifier.size(24.dp), tint: Color = LocalContentColor.current) {
    val vector = remember(name) {
        val path = when (name) {
            "download" -> "M19,9h-4V3H9v6H5l7,7z M5,18v3h14v-3z"
            "storage" -> "M3,3h18v5H3z M3,10h18v5H3z M3,17h18v4H3z"
            "guide" -> "M4,3h16v18H4z M7,6v2h10V6z M7,11v2h10v-2z M7,16v2h6v-2z"
            "pause" -> "M6,4h4v16H6z M14,4h4v16h-4z"
            "play" -> "M7,3v18l14,-9z"
            "add" -> "M11,4h2v7h7v2h-7v7h-2v-7H4v-2h7z"
            "check" -> "M9,16.2L4.8,12l-1.4,1.4L9,19 21,7l-1.4,-1.4z"
            else -> "M5,2h9l5,5v15H5z M13,3v5h5z M8,12v2h8v-2z M8,16v2h6v-2z"
        }
        ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f).addPath(
            pathData = PathParser().parsePathString(path).toNodes(), fill = SolidColor(Color.Black)
        ).build()
    }
    Icon(vector, contentDescription = null, modifier = modifier, tint = tint)
}
