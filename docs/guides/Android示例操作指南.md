# Android 示例操作指南

[文档中心](../README.md)

本页供试用示例 App 的读者查看下载、续传、导出和后台操作。

## 直接开始

在 Android Studio 运行 `sample`，或安装
`platform/android/sample/build/outputs/apk/debug/fetchkit-sample-<VERSION>-debug.apk`。
`<VERSION>` 取自仓库根目录，包含预发布后缀。

点「新建下载」，选择一个或多个镜像预设，再点开始。下载列表会显示各任务进度。
镜像返回 HTTP 403 时，可在详情选择「更新地址并重试」，原资源身份和校验约束仍然有效。
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
| 事件 / 分页 | 持续 observe 并分发订阅事件，日志仅保留最新 60 条非进度事件；任务按 100 条加载，可加载更多                                                    |
| 后台许可    | 新建单文件选择系统后台调度及非计费网络约束；API 34+ UIDT，较低 API 普通有限 JobScheduler，系统决定执行机会                                    |

## 后台下载

新建单文件时选择系统后台调度，可指定非计费网络。API 34+ 使用 UIDT，低版本由普通 JobScheduler
提供有限、可中断的执行机会。API 28+ 使用系统分配的网络，启动前检查约束。

- 后台队列最多 15 个预设槽位和 1 个自定义槽位。同一预设及版本重复提交、或槽位已满时会拒绝。
  后台一次运行一个下载，其他许可交回系统。切换后台引擎时，前台任务保留检查点，需手动恢复。
- Job extras 保存预设 ID 和非敏感约束，或自定义任务的随机许可令牌。自定义后台 URL 仅存在进程内，
  只接受没有 userinfo、query 或 fragment 的公开地址。进程重开后需重新绑定。
- 暂停或取消最后一个订阅时，先禁止 Job 续跑，待 SDK 操作完成后撤销 Job。系统停止或网络失效时，
  异步关闭引擎并尽力提交检查点，等系统再次授权后恢复。API 34+ 分配新网络时，等旧引擎关闭后再重开。
- 用户强制停止后，下次合法执行机会才可恢复。进程终止、系统停止、重启和掉电需分别记录验证证据。

## 文件导出与重新绑定

已完成且可用的下载可点「导出文件」，在系统文件选择器中指定保存位置。
返回时会显示写入字节数。复制期间可点「取消导出」，失败或取消后请检查目标是否留有部分内容。
示例保留目标供用户检查，不自动删除文档提供方返回的
Uri。接口行为见 [文件导出](../reference/AndroidSDK接口与行为.md#文件导出)。

展开任务详情和打开重新绑定面板时，示例按持久 ID 主动查询该资源。下载列表仍分页刷新。

前台请求按 ResourceIdentity 分别保存，恢复/暂停不会引用另一个输入框的内容。 宿主仅在
`noBackupFilesDir/sample-public-requests.json` 原子保存固定预设 ID 和非敏感请求约束，所有文件操作在有界后台
executor 上执行。 SDK 的 `filesDir/fetchkit`、记录格式和备份排除规则保持原有语义。 自定义 URL
仅存当前进程；界面和事件日志不显示完整地址、凭证或原始异常消息。 暂停/关闭是异步操作，UI
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
`SampleDashboardTest` 在不启动下载的情况下验证默认预设、多选、空选择和页面导航。
「新建下载」按钮显式提供可访问名称，设备回归同时检查标签和实际打开面板的行为；Release 启动驱动
通过当前应用节点的文字或可访问描述识别入口。
`SampleControlsTest` 使用设备内受控 HTTP 服务器，验证并发资源的逐行控制、确认检查点后的真实 Range 请求、
独立订阅取消、可信摘要、读取租约删除保护、pin 与过期，不依赖公共大文件服务。
`SampleDownloadTest` 保留带 query URL 的提交与本地下载/读取回归。 逐类运行并关闭配置缓存，核对 XML
中的具体类和用例数量；当前环境的多类筛选实际只执行首个类，不能仅凭 Gradle 成功判断全部用例通过。 API
34+ UIDT、物理设备矩阵、进程终止/重启/掉电仍须各自的专门设备证据，不能由以上测试推导。
