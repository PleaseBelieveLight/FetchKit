# Android 示例操作指南

[文档中心](../README.md)

sample 使用真实 Kotlin/JNI SDK，不模拟进度，不添加新的运行依赖。入口为 `MainActivity`；
`download_screen.kt` 负责 Compose 展示，`SampleDownloads` 管理宿主生命周期、请求和订阅，下载及存储策略仍在
C++。

## 直接开始

在 Android Studio 运行 `sample`，或安装
`platform/android/sample/build/outputs/apk/debug/sample-debug.apk`。 点击「新建下载」，默认选中清华镜像的
Ubuntu 24.04.5 Netboot；可多选以下预设，无需输入地址：

| 预设                 | URL                                                                                                                       |
|----------------------|---------------------------------------------------------------------------------------------------------------------------|
| 清华镜像 · Netboot   | `https://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/24.04.5/ubuntu-24.04.5-netboot-amd64.tar.gz?utm_source=chatgpt.com` |
| 清华镜像 · WSL       | `https://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/24.04.5/ubuntu-24.04.5-wsl-amd64.wsl?utm_source=chatgpt.com`        |
| 阿里云镜像 · Netboot | `https://mirrors.aliyun.com/ubuntu-releases/24.04.5/ubuntu-24.04.5-netboot-amd64.tar.gz?utm_source=chatgpt.com`           |

镜像站点可能限速、拒绝请求或改变 Range/validator 支持；应根据任务详情判断实际结果。
实际文件大小以服务端响应为准，预设没有预填未经验证的长度或 SHA-256，由用户明确点击开始后才下载。

示例的前台和后台请求显式携带 `User-Agent: FetchKitSample/1.0`，表明宿主应用身份。 调试设备曾在清华
Netboot 地址复现默认 `okhttp/5.5.1` 的 GET 返回 403，而同一网络、链接和 Range 使用示例应用标识返回
206；去掉查询参数仍然 403，HEAD 则可返回 200，因此仅检查 HEAD 不能验证下载。
该结果反映当时镜像站的客户端/网段风控，不能保证后续所有网络均可访问。 若已有 HTTP 403
的失败记录，请在存储页移除该失败资源后重新创建；此类错误不参与 SDK 普通自动重试。

「使用自定义地址」是可选入口。高级选项提供业务版本、单文件预期字节数、可信 SHA-256 和创建时长期保留。
批量任务不共用同一个摘要；需要校验时逐个创建。HTTP 只在 Debug 示例中允许，Release 沿用 SDK/平台 HTTPS
策略。 新建/重新绑定面板的最大高度受顶部安全区域约束，距状态栏或刘海安全边界额外留出 24
dp；内容超出时内部滚动，输入时适配键盘。

## 能力与操作

| 能力        | 操作与观察                                                                                                                                    |
|-------------|-----------------------------------------------------------------------------------------------------------------------------------------------|
| 多文件      | 多选预设开始，观察最多 3 个活动下载及各自的进度、速度和预计剩余时间；未知长度显示不定进度                                                     |
| 暂停 / 续传 | 暂停后等待检查点确认，再恢复；详情显示 received、committed、generation、HTTP 和请求数。SDK 校验服务端后决定 Range 续传或安全重下              |
| 批量控制    | 下载页「全部暂停 / 全部恢复」；只恢复已提供原请求且 SDK 标识可恢复的任务                                                                      |
| 重开恢复    | 离开应用保存检查点，重新打开后手动恢复。预设由宿主目录重建；自定义任务点击「重新绑定」并填原长度/摘要约束                                     |
| 独立订阅    | 详情添加第二个订阅，再取消一个，其他订阅继续；最后一个显式取消清理未完成数据。释放最后一个则暂停并保留检查点。演示页每个资源最多持有 4 个订阅 |
| 完整性      | 普通 Completed 显示「传输完整 · 未验证可信摘要」，正确 SHA-256 显示「SHA-256 已验证」；错误摘要不会发布。非可恢复错误需移除后重新创建         |
| 重试        | 断网或可重试 HTTP 错误触发有限重试，详情展示 retryCount、nextRetryAt；恢复不会重置持久化重试预算                                              |
| 资源读取    | 已发布可用资源点击「租约读取」，存储页展示前 64 字节十六进制并保持租约。释放前删除/淘汰受保护，离开页面关闭 engine 时也释放租约               |
| pin / 过期  | 存储页切换长期保留、10 分钟后过期、立即过期或清除到期时间。过期拒绝新 acquire，已有 lease 继续有效                                            |
| 配额 / LRU  | 展示 used、reserved、charged、metadata、隔离/未知版本/孤立数量，确认后尝试 trimTo(512 MiB)，尊重活动/pin/lease 保护，失败也可能已回收部分资源 |
| 暂存清理    | collectStaging 仅回收超过配置的 7 天且未受保护的有记录暂存；不是删除所有暂停任务                                                              |
| 安全移除    | 停止且未 pin/leased 的资源可确认删除，调用 SDK remove，不通过文件路径自行删除                                                                 |
| 事件 / 分页 | 持续 observe 并分发订阅事件，日志仅保留最新 60 条非进度事件；任务按 100 条加载，可加载更多                                                    |
| 后台许可    | 新建单文件选择系统后台调度及非计费网络约束；API 34+ UIDT，较低 API 普通有限 JobScheduler，系统决定执行机会                                    |

后台一次排队一个系统任务，新排队会替换旧的待执行任务；执行中的后台任务需要先暂停/取消。 后台启动时切换到系统许可的
Network 并关闭原 engine，其他前台任务保留检查点，需用户重新恢复。 镜像资源通过固定预设 ID 重建带参数
URL；自定义后台 URL 不允许凭证、query 或 fragment。 强制停止后不承诺自动重启；正常离开、进程终止、系统重启和掉电的验证证据应分别报告。

## 请求与生命周期

前台请求按 ResourceIdentity 分别保存，恢复/暂停不会引用另一个输入框的内容。 宿主仅在
`noBackupFilesDir/sample-public-requests.json` 原子保存固定预设 ID 和非敏感请求约束，所有文件操作在有界后台
executor 上执行。 SDK 的 `filesDir/fetchkit`、记录格式和备份排除规则保持原有语义。 自定义 URL
仅存当前进程；界面和事件日志不显示完整地址、凭证或原始异常消息。 暂停/关闭是异步操作，UI
不阻塞主线程等待。生命周期代次防止关闭后的请求/查询回调重新更新旧界面或持有新租约。

当前配置为并发 3、队列 1024、最多重试 3、单文件 2 GiB、总配额 4 GiB、检查点 4 MiB 或 1 秒、暂存保留 7 天。
需要大于 2 GiB 的示例时应明确调整单文件与配额预算；不要把此示例默认限制误认为 SDK 的 64 位偏移上限。
请求头、鉴权刷新 RequestProvider、超时及网络绑定的接入示例见 [Android API](../reference/android-api.md)。

## 验证

```powershell
.\gradlew.bat :sdk:assembleDebug :sdk:testDebugUnitTest :sample:assembleDebug :sample:testDebugUnitTest
$sampleTestClasses = @('SdkIntegrationTest', 'SampleDownloadTest', 'SampleControlsTest', 'SampleDashboardTest')
foreach ($sampleTestClass in $sampleTestClasses) {
    .\gradlew.bat :sample:connectedDebugAndroidTest "-Pandroid.testInstrumentationRunnerArguments.class=com.luofelix.fetchkit.sample.$sampleTestClass" --no-configuration-cache
    if ($LASTEXITCODE -ne 0) { throw "Device tests failed: $sampleTestClass" }
}
```

`SampleModelsTest` 检查预设身份、查询参数、版本、可选摘要及 64 位格式化。
`SampleDashboardTest` 在不启动下载的情况下验证默认预设、多选、空选择和页面导航。
`SampleControlsTest` 使用设备内受控 HTTP 服务器，验证并发资源的逐行控制、确认检查点后的真实 Range 请求、
独立订阅取消、可信摘要、读取租约删除保护、pin 与过期，不依赖公共大文件服务。
`SampleDownloadTest` 保留带 query URL 的提交与本地下载/读取回归。 逐类运行并关闭配置缓存，核对 XML
中的具体类和用例数量；当前环境的多类筛选实际只执行首个类，不能仅凭 Gradle 成功判断全部用例通过。 API
34+ UIDT、物理设备矩阵、进程终止/重启/掉电仍须各自的专门设备证据，不能由以上测试推导。
