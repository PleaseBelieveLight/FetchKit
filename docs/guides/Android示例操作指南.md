# Android 示例操作指南

[文档中心](../README.md)

本页供试用示例 App 的读者查看下载、续传、导出和后台操作。

## 直接开始

在 Android Studio 运行 `sample`，或安装
`platform/android/sample/build/outputs/apk/debug/fetchkit-sample-<VERSION>-debug.apk`。
`<VERSION>` 取自仓库根目录，包含预发布后缀。

点「新建下载」，选择一个或多个镜像预设，再点开始。下载列表会显示各任务进度。
默认选择中科大镜像的 Alpine Standard（270 MiB），也可选择南大镜像的 Alpine Virtual（67 MiB）。
展开「更多镜像 · 大文件与快捷验证」，可使用南大 Alpine Extended（约 1.09 GiB）和原 Ubuntu 预设做大文件续传，
也可用清华 Alpine 校验文件（105 B）和阿里云 Minirootfs（约 3.5 MiB）快速验证查询和导出。
自定义地址可选填业务版本、长度和可信 SHA-256。需要校验多个文件时，逐个创建任务。
HTTP 仅在 Debug 示例中允许。

公开分发 APK 见 [演示 APK 的公开签名](../release/SDK打包与发布.md#演示-apk-的公开签名)。
Debug 与 Release 签名不同，换装前注意保留数据。

## 能力与操作

| 能力        | 操作与观察                                                                                                                                    |
|-------------|-----------------------------------------------------------------------------------------------------------------------------------------------|
| 多文件      | 多选预设开始，观察最多 3 个活动下载及各自的进度、速度和预计剩余时间；未知长度显示不定进度                                                     |
| 暂停 / 续传 | 暂停后等待检查点确认，再恢复；详情显示 received、committed、generation、HTTP 和请求数。SDK 校验服务端后决定 Range 续传或安全重下              |
| 批量控制    | 下载页「全部暂停 / 全部恢复」；只恢复已提供原请求且 SDK 标识可恢复的任务                                                                      |
| 重开恢复    | 离开应用保存检查点，重新打开后手动恢复。预设由宿主目录重建；自定义任务点击「重新绑定」并填原长度/摘要约束                                     |
| 独立订阅    | 详情添加第二个订阅，再取消一个，其他订阅继续；最后一个显式取消清理未完成数据。释放最后一个则暂停并保留检查点。演示页每个资源最多持有 4 个订阅 |
| 完整性      | 普通 Completed 显示「传输完整 · 未验证可信摘要」，正确 SHA-256 显示「SHA-256 已验证」；错误摘要不会发布。非可恢复错误需移除后重新创建         |
| 重试        | 断网或可重试 HTTP 错误触发有限重试；鉴权/网络/超时/HTTP 失败可更新地址并重试，保留检查点、原校验约束及预算；活动任务须先暂停                  |
| 资源读取    | 已发布可用资源点击「租约读取」，存储页展示前 64 字节十六进制并保持租约。释放前删除/淘汰受保护，离开页面关闭 engine 时也释放租约               |
| pin / 过期  | 存储页切换长期保留、10 分钟后过期、立即过期或清除到期时间。过期拒绝新 acquire，已有 lease 继续有效                                            |
| 配额 / LRU  | 展示 used、reserved、charged、metadata、隔离/未知版本/孤立数量，确认后尝试 trimTo(512 MiB)，尊重活动/pin/lease 保护，失败也可能已回收部分资源 |
| 暂存清理    | collectStaging 仅回收超过配置的 7 天且未受保护的有记录暂存；不是删除所有暂停任务                                                              |
| 安全移除    | 停止且未 pin/leased 的资源可确认删除，调用 SDK remove，不通过文件路径自行删除                                                                 |
| 快照 / 分页 | 展开详情可观察协程 StateFlow 实时快照，能力指南保留最近 60 条状态变化。任务按 100 条加载，可加载更多                                          |
| 后台许可    | 新建单文件选择系统后台调度及非计费网络约束；API 34+ UIDT，较低 API 普通有限 JobScheduler，系统决定执行机会                                    |

## 后台下载

新建单文件时选择系统后台调度，可指定非计费网络。API 34+ 使用 UIDT，低版本由普通 JobScheduler
提供有限、可中断的执行机会。API 28+ 使用系统分配的网络，启动前检查约束。
下载页的「后台队列」显示尚未生成下载记录的待调度项，以及正在执行的任务和网络条件。

- 后台队列最多 15 个预设槽位和 1 个自定义槽位。同一预设及版本重复提交、或槽位已满时会拒绝。
  后台一次运行一个下载，其他许可交回系统。切换后台引擎时，前台任务保留检查点，需手动恢复。
- Job extras 保存预设 ID 和非敏感约束，或自定义任务的随机许可令牌。自定义后台 URL 仅存在进程内，
  只接受没有 userinfo、query 或 fragment 的公开地址。进程重开后需重新绑定。
- 暂停或取消最后一个订阅时，先禁止 Job 续跑，待 SDK 操作完成后撤销 Job。系统停止或网络失效时，
  异步关闭引擎并尽力提交检查点，等系统再次授权后恢复。API 34+ 分配新网络时，等旧引擎关闭后再重开。
- 用户强制停止后，下次合法执行机会才可恢复。进程终止、系统停止、重启和掉电需分别记录验证证据。

## 即时查询、换链与文件导出

下载页「按 ID 查询」可输入完整的持久 ID，直接读取分页外的单条记录。任务详情的「即时查询任务」
可切换持久 ID 与资源身份两种查询方式，长按结果可复制完整 ID。详情内的实时快照随订阅更新。

能力指南中点「准备换链演练」，提交预填的缺失地址。返回下载页，等待出现 HTTP 404，再点
「更新地址并重试」。恢复面板已预填可用的小文件地址，提交后可观察同一任务进入新运行代次。
对真实失效链接，可在恢复面板输入同一资源的新地址。已暂停的任务也可在详情点「更新地址并恢复」，
观察 SDK 是否从已保存的检查点续传。

新建或换链的高级选项可填写「鉴权刷新地址」。SDK 收到 401/403 时由示例 provider 提供该地址；
可额外勾选 404/410。两个地址应对应同一资源，恢复时仍需满足原长度和摘要约束。

已完成且可用的下载可点「导出文件」，在系统文件选择器中指定保存位置。
返回时会显示写入字节数。复制期间可点「取消导出」，失败或取消后请检查目标是否留有部分内容。
示例保留目标供用户检查，不自动删除文档提供方返回的
Uri。接口行为见 [文件导出](../reference/AndroidSDK接口与行为.md#文件导出)。

前台请求按 ResourceIdentity 分别保存，恢复/暂停不会引用另一个输入框的内容。 宿主仅在
`noBackupFilesDir/sample-public-requests.json` 原子保存固定预设 ID 和非敏感请求约束，所有文件操作在有界后台
executor 上执行。 SDK 的 `filesDir/fetchkit`、记录格式和备份排除规则保持原有语义。 自定义 URL
和鉴权刷新地址仅存当前进程，完整地址只在填写地址的表单中显示。 暂停/关闭是异步操作，UI
不阻塞主线程等待。生命周期代次防止关闭后的请求/查询回调重新更新旧界面或持有新租约。

当前配置为并发 3、队列 1024、最多重试 3、单文件 2 GiB、总配额 4 GiB、检查点 4 MiB 或 1 秒、暂存保留 7 天。
需要大于 2 GiB 的示例时应明确调整单文件与配额预算；不要把此示例默认限制误认为 SDK 的 64 位偏移上限。
请求头、鉴权刷新
RequestProvider、超时及网络绑定的接入示例见 [Android API](../reference/AndroidSDK接口与行为.md)。

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
`SampleDashboardTest` 在不启动下载的情况下验证默认预设、多选、空选择、页面导航和未知持久 ID 查询。
「新建下载」按钮显式提供可访问名称，设备回归同时检查标签和实际打开面板的行为；Release 启动驱动
通过当前应用节点的文字或可访问描述识别入口。
`SampleControlsTest` 使用设备内受控 HTTP 服务器，验证换链后的真实 Range、两种即时查询、实时快照、
独立订阅取消、读取租约删除保护、pin 与过期。API 29+ 还将导出写入测试自建的 content Uri 并逐字节核对。
`SampleDownloadTest` 保留带 query URL 的提交与本地下载/读取回归。 逐类运行并关闭配置缓存，核对 XML
中的具体类和用例数量；当前环境的多类筛选实际只执行首个类，不能仅凭 Gradle 成功判断全部用例通过。 API
34+ UIDT、物理设备矩阵、进程终止/重启/掉电仍须各自的专门设备证据，不能由以上测试推导。
