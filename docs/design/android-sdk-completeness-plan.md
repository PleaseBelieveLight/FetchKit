# Android SDK 功能补齐实施计划

[文档中心](../README.md) · [设计基线](v1-design.md) · [首版落地计划](v1-implementation-plan.md)

日期：2026-11-02。评估基线：提交 `4997567`，源码版本 `0.2.0-beta.3`。
本计划承接现有 C++、Kotlin/JNI 和 sample，描述后续工作；接口名称均为草案，完成对应实现与验收前不属于现有
API。
当前契约仍以 [主机 API](../reference/host-api.md)、[Android API](../reference/android-api.md)
和 [传输契约](../reference/transport-contract.md) 为准。实施进度与实测以末尾清单和对应阶段报告为准。

## 1. 目标与交付顺序

先完成用户能够观察到的下载恢复与状态查询，再补齐 Android 执行、导出和 Kotlin 接入体验。
C++ 继续负责共享任务、续传、重试、存储和校验；Android 负责系统执行机会、网络约束和目标 Uri。

| 阶段 | 优先级  | 交付内容                                                 | 前置依赖           | 初步工作量 |
|------|---------|----------------------------------------------------------|--------------------|------------|
| A0   | P0      | 固定状态转换、请求替换、预算和生命周期契约，建立失败复现 | 无                 | 1～2 人日  |
| A1   | P1      | 过期链接、鉴权失败后的请求替换与保留检查点重试           | A0                 | 5～8 人日  |
| A2   | P1      | 按持久 ID / 资源身份主动查询单个任务                     | A0；与 A1 集成验收 | 2～3 人日  |
| A3   | P2      | 后台执行协调与任务网络条件                               | A1、A2             | 5～8 人日  |
| A4   | P2      | 文件导出、协程和状态观察适配                             | A1、A2；可分别交付 | 5～8 人日  |
| A5   | P1 / P2 | 按交付范围做设备、外部接入与打包验收                     | 对应功能阶段       | 3～5 人日  |

一名熟悉项目的开发者顺序实施约 21～34 人日，包含相关回归与文档，不含设备获取和外部环境等待；
A0 后按实际失败场景调整估算。先交付 A0～A2 并完成对应 A5 检查，形成第一批可独立使用的改进；
A3、A4 分批进入后续候选版本，不等待所有便利功能完成才交付核心修复。

本轮不纳入多连接分段、HLS/DASH、DRM、P2P、解压安装、32 位 ABI、新 Android 网络后端和完整 HTTP 缓存。
优先级、限速与镜像池列为后续需求，不改变当前 FIFO 调度。URL 更新可用于换 CDN，但不承诺自动镜像择优。

## 2. 已确认的问题与处理方向

| 场景                              | 当前原因 / 边界                                                           | 本轮处理                                                     |
|-----------------------------------|---------------------------------------------------------------------------|--------------------------------------------------------------|
| 鉴权刷新失败后，换链接仍无法继续  | `resumable` 不接受 `Failed + AuthRequired`，同身份再次 enqueue 也不会启动 | 增加显式的新请求重试事务，保留合法检查点                     |
| 新增订阅仍可能使用旧链接          | 共享任务选择第一个有效订阅；新订阅不会覆盖旧请求                          | 明确本轮请求来源，避免依赖订阅插入顺序                       |
| 获取签名 URL 的接口暂时断网       | Kotlin provider 异常统一映射为 AuthRequired                               | 提供受限的类型化 provider 失败，保留旧异常默认映射           |
| URL 过期返回 404/410              | 默认刷新只响应 401/403                                                    | 默认行为保持；允许宿主显式声明额外的刷新 HTTP 状态           |
| URL 过期返回 HTTP 200 错误页面    | 通用下载器无法凭状态码辨认业务内容                                        | 关键资源使用可信 SHA-256；覆盖错误页面不能通过摘要校验的用例 |
| 只知道 persistentId，想查一个任务 | Kotlin 只有订阅 query 与列表分页                                          | 增加无订阅、无执行副作用的单任务查询                         |
| 网络恢复后下载继续、导出用户文件  | 目前需要宿主自行组织多个异步操作                                          | 增加可验证的 Android 协调示例和有界复制接口                  |

`expiresAtMs` 始终表示资源可用期限，与签名 URL 有效期分开。URL/token 不进入 SDK 持久化记录；
provider 应在每轮 Initial 阶段按业务引用取得当前有效请求，SDK 不解析 URL 查询参数来推断签名过期时间。

## 3. A0：先固定新增契约和回归场景

A0 的具体接口/状态表和回归要求固定在 [新请求重试契约](request-retry-contract.md)，后续实现以该契约为准。

实施落点：`include/fetchkit/fetchkit.h`、`src/core/engine.*`、`src/core/download_session.cpp`、
Android `Models.kt` / `DownloadEngine.kt` / JNI，以及现有测试服务器与单元测试。

1. 在现有 CTest 和 Android 集成测试中复现“401/403 → 刷新失败 → 新凭证可用 → 同身份恢复失败”。
   同时记录正常首次刷新、暂停续传、共享订阅和重试预算的现有行为，修复不得破坏这些行为。
2. 固定请求替换的准入状态、身份与约束校验、共享可见性、provider 所有权、取消/释放屏障和新 generation 规则。
3. 明确运行许可、普通网络重试预算和鉴权刷新预算分别如何变化；先写预期状态转换，再调整可恢复判断。
4. 新 API 以增量方式加入 C ABI、C++ 包装、JNI 和 Kotlin；不重排现有状态/错误枚举。
   新选项使用有大小/版本边界的结构；Android bridge 编码、参数上限和 R8 规则同步维护。

退出条件：新增行为、旧行为及故障出口有可执行断言；API 注释和契约文档能解释共享任务与 provider 的实际生命周期。
失败复现单独运行，不以放宽断言作为修复。

## 4. A1：完成换链与失败恢复

### 4.1 显式使用新请求重试

草案入口：`DownloadSubscription.retry(request: DownloadRequest): CompletableFuture<Unit>`，
对应新增 C ABI 和 C++ 包装。一次调用完成“校验并替换调用订阅的请求 + 选择本轮请求来源 + 授予执行许可”，
避免宿主串联多个操作时被其他订阅插入。

- 仅接受已停止的 Paused，或失败原因为 AuthRequired、Network、Timeout、HttpStatus 的 Failed。
  HttpStatus 的显式重试可用于宿主确认已修正的 404/410 等失败，不因此把这些状态加入普通自动重试集合。
  Running、Queued、RetryWaiting、Pausing、Verifying、Publishing 返回 Busy；需要更新正在运行的任务时，先等待暂停完成。
  Completed 不作为失败重试，Cancelled/Removed 也不通过旧订阅复活。
- namespace/key/version/representation、长度、摘要、requireDigest 和 addressReference 必须保持原约束。
  URL、headers、provider 可以更新；expiresAtMs 必须与当前记录一致，pinOnCreate 不重新应用。
  pin/资源过期变更继续使用现有存储策略接口。
- 只替换调用者自己的订阅请求；该订阅成为本 generation 的首选请求来源，其他订阅观察同一个共享任务。
  首选订阅失效时按明确的有效订阅规则交接，绝不再访问已释放 provider；交接无可用请求时停止并给出可观察结果。
- provider 的执行与等待不持 SDK 锁；释放、关闭、取消与请求来源交接复用现有屏障和 generation 隔离。
  原子准入失败时不发布新请求来源、不推进 generation、不丢弃旧检查点，并回滚预留/保护状态。
- 新执行增加 generation，鉴权刷新获得该轮最多一次预算；已用普通重试次数及 Retry-After 截止时间保留。
  用户显式重试可给予一次新的执行机会，已耗尽的普通预算不会因此重新获得自动重试次数。
- 保留 committedOffset、ETag 和合法暂存前缀；新地址仍经过现有 Range/If-Range、长度和摘要校验。
  服务端不接受续传或资源变化时按现有规则安全重下，不宣称换链一定能保留进度。
- 原 `enqueue` / `attach` 不改为“最后请求覆盖”；原 `resume` 契约保持，文档引导更换请求时使用新入口。
  Future 成功只表示执行已获准，后续通过 query/observe/awaitStopped 判断结果。

### 4.2 刷新策略与 provider 错误

- 默认继续对 401/403 触发刷新；增加每请求的显式刷新状态白名单草案，字段数量有界，额外状态首批仅允许
  404/410。
  404/410 未配置时保留普通 HTTP 失败，不自动猜测资源已过期。所有刷新状态共用每 generation 一次预算。
- 初始、鉴权刷新和重定向仍由核心决定；跨 origin 不隐式传递旧凭证，HTTPS 不降级。
  RequestContext 继续只暴露安全上下文，不向业务开放未脱敏响应体。
- 增加类型化 provider 失败草案，首批只允许 Network、Timeout、AuthRequired、Cancelled；
  Kotlin/JNI/C ABI 对齐取消与重试分类。旧 provider 抛出的未分类异常保留 AuthRequired 映射及安全诊断。
  provider 超时仍是协作边界，不新增无限异步请求队列，也不声称可强行中断任意宿主代码。
- 不增加 URL/token 持久化。能复用现有 v2 鉴权/重试字段时不升级记录格式；如确需扩展，先补充
  [记录 v2 契约](../reference/storage-record-v2.md) 与旧记录/未知字段测试，再实现迁移。
- sample 增加失败后重新绑定并重试的入口；保留原业务身份和完整性约束，不要求先删除已有数据。
  未配置摘要的资源仍明确显示 TransportComplete；不把 MIME 类型检查当作内容真实性验证。

### 4.3 A1 验收

复用 `tests/unit/download_session_test.cpp`、`tests/unit/scheduler_test.cpp`、协议服务器和
Android `DownloadIntegrationTest.kt`，至少覆盖：

1. 401、403 分别刷新成功；连续拒绝仅刷新一次；无 provider 的失败可观察。
2. 已提交前缀在鉴权失败后仍保留；retry 新链接实际发送精确 Range/If-Range，并通过租约全文件 SHA-256。
3. 原订阅持有旧链接、第二订阅执行 retry，服务器确认本轮使用新链接；首选 provider 取消/释放时无迟到访问。
4. provider 暂时网络失败参与有限重试，AuthRequired 不循环重试；超时、取消和未知异常分类符合契约。
5. 404/410 默认不刷新，显式配置后才刷新；HTTP 200 错误页面无法通过指定可信摘要。
6. 同域/跨域换链、凭证清除、禁止降级；ETag/长度变化安全重下；约束冲突失败后原任务仍可查询和恢复。
7. 重试预算耗尽、Retry-After 等待、重开重绑不绕过限制；新 generation 事件不会覆盖到旧轮次。
8. retry 与 pause/cancel/release/close 的确定性交错；只有一个 writer，停止/关闭后 provider 和网络回调均满足屏障。

## 5. A2：直接查询单个任务

草案接口：`DownloadEngine.query(persistentId)` 与 `query(identity)`，异步返回 `StoredTask`；不存在时使用
`FetchKitException(Status.NotFound)`，不增加“未找到任务”伪状态。C ABI 提供同步查询，Kotlin 在有界 IO
工作池调用。

- 复用身份规范化、快照装配和资源可用性规则；优先使用现有资源索引，不通过遍历 listTasks 所有分页寻找目标。
- 不创建订阅、不运行 provider、不增加 generation、不启动 I/O 下载，也不改变 LRU 访问顺序。
  无订阅的查询结果 taskId 为 0；persistentId 表示资源身份，不承诺返回某一订阅的局部 Cancelled 状态。
- 运行中的资源合并实时进度；未绑定的持久化资源返回可恢复记录视图。available 仍受过期、损坏和淘汰影响，
  query 不代替 acquire 的校验与租约准入；磁盘检查继续在 Kotlin IO 线程执行。
- 保留 `subscription.query()` 和 `listTasks()`。不给 Android 增加主线程阻塞查询包装；需要同步使用时明确仅后台线程等待
  Future。
  缓存中的 UI 状态属于观察结果，必须与主动查询的新快照区分。
- sample 重开后的单任务详情、失败处理和重新绑定采用直接查询；任务列表继续分页。

验收覆盖：中文/补充平面身份、非法 ID、NotFound、运行/暂停/失败/完成/过期/Removed、进程重开、无 provider
查询、
query 与 remove/trim/close 的竞态，以及大量任务中单项查询不枚举所有分页。并发删除允许已在线性化点获得的快照或
NotFound，
关闭竞争允许已进入操作完成或 Closed，不通过严格要求单一竞态结果制造脆弱测试。

## 6. A3：Android 执行与网络协调

先在 sample 落地可测试的协调器；可复用边界明确后再抽取独立可选适配模块，不提前创建空模块，
不向基础 SDK 加入 WorkManager、Compose 或通知依赖。

- 协调器按 persistentId 管理宿主期望运行状态、系统许可与网络条件，替换 sample 只能排一个固定 Job ID
  的方式。
  资源与系统 job 的映射、并发数和恢复目录均有界；系统调度仅提供执行机会，不复制 HTTP 重试/下载策略。
- 用户暂停、系统停止与等待网络分别记录原因。网络恢复只恢复宿主仍期望运行且已获许可的任务，不覆盖用户暂停或取消。
  等待约束期间不启动 HTTP，因此不消耗普通重试次数；网络错误已经发生时不倒退或清零核心预算。
- 提供任意可用网络、非计费网络两种约束。首批沿用 engine 级 Network：更换网络须停止并提交检查点、关闭
  engine、
  用新 Network 重开并重绑。只恢复仍应运行的任务，不承诺一个 engine 同时使用不同 Network。
- API 34+ 的用户发起长传输使用 UIDT，通知和调度时机按
  [Android 官方 UIDT 文档](https://developer.android.com/develop/background-work/background-tasks/uidt)
  实现。
  API 24～33 保留有限普通 JobScheduler 示例，明确可中断；旧版长期传输适配另立工作包并验证对应平台限制。
- Job extras/宿主恢复清单只保存业务引用、身份和非敏感约束；签名 URL、凭证和 provider 不持久化。
  由宿主 resolver 重建请求，重开后不自行下载未获执行许可的任务。
- onStopJob 发起异步停止；系统再次给予机会后重建 provider。强制停止不承诺自动重启。

验收覆盖：多任务排队不互相覆盖、网络断开/非计费约束变化/Network 替换、用户暂停后切网不误恢复、
Job 停止及重新执行、进程终止后重绑、通知生命周期；分别记录 API 24、中间 API、34+、仓库目标 API 和物理
arm64。
允许 API 覆盖角色重合，但模拟器不替代真机；长期旧版传输未实现时不把这项标为完成。

## 7. A4：导出与 Kotlin 接入

### 7.1 导出已完成资源

- 草案提供 engine 按 persistentId 复制到宿主 OutputStream 的异步接口；内部 acquire 独立
  lease，复制结束或失败均释放。
  使用固定大小缓冲和有界执行资源，不缓存整文件，不从 API 暴露内部 object/part 路径。
- 复制任务不得在公共两线程工作池内同步等待同池的逐块 Future；采用独立有界执行通道或非阻塞异步组合。
  返回值报告复制字节数与结构化失败，取消导出只停止复制，不取消共享下载。
- 明确流所有权：宿主传入的流由宿主关闭；Uri 帮助方法自行打开的流自行关闭。
  engine 关闭后已取得 lease 的复制可继续；资源在导出期间受租约保护。
- sample 通过 ACTION_CREATE_DOCUMENT 选择目标，参照
  [Android SAF 文档](https://developer.android.com/training/data-storage/shared/documents-files)；
  覆盖 Uri 授权撤销、目标写满、写入失败、用户取消和部分目标文件的处理。只清理本次创建且可确认归属的目标。
  MediaStore、分享 Uri 和流式播放器适配后置，不将“导出至 SAF”扩大为所有外部存储场景。

### 7.2 可选协程与快照观察

- 增加 suspend await 与快照 StateFlow 适配；只有实施该功能时才增加可选模块和锁定依赖，基础 SDK 保留
  Future API。
- 协程取消不隐式调用共享任务 cancel。open/enqueue/acquire 等创建资源的操作在取消与完成竞争中，
  必须交付所有权或显式释放成功创建的 engine/订阅/lease，不能丢失 native 句柄。
- 适配现有每订阅仅注册一次 observer 的限制：由一个有界观察桥接多个 collector，避免重复 collect 触发第二次
  observe。
  快照流可合并中间进度；需要完整状态事件的消费者继续使用既有事件 API，不承诺 StateFlow 保留所有转换。
- 零 collector 时不向 UI 分发，桥接仍排空核心事件且只保留最新快照；owner 释放时关闭观察，避免重新进入页面时重复注册或积压事件。
- 定义初始 query 与后续事件的排序，使用 persistentId、generation 和当前轮次信息防止快照回退；
  collector 离开后停止分发，订阅/lease 的释放由明确的 owner 管理。

验收覆盖：零字节与大文件复制、取消/关闭/删除竞争、慢输出和异常输出、多个 collector、重复进入页面、
创建 Future 的完成/取消竞争和慢消费者有界性；示例显示的速度/ETA 继续由宿主按进度采样计算。

## 8. A5：验证、文档和交付

命令、环境准备与参数以 [开发指南](../guides/development.md)、[测试说明](../../tests/README.md)
和 [打包指南](../release/packaging.md) 为权威位置；本计划只列本轮检查选择。

| 改动               | 必需检查                                                                                                                                         |
|--------------------|--------------------------------------------------------------------------------------------------------------------------------------------------|
| A1 核心和 provider | CTest `c_api`、`download_session`、`scheduler_storage`、`download_protocol`、`m3_protocol`；涉及记录提交/恢复时加 `storage`、`download_recovery` |
| A2 查询            | C ABI / C++ 查询用例、存储与调度相关用例、Kotlin/JNI 接入和关闭竞争                                                                              |
| A1/A2 Android      | SDK 构建/单测，正式 Kotlin/JNI/OkHttp 协议驱动；新增场景同步驱动的预期测试数量                                                                   |
| A3                 | sample 构建/单测、对应 instrumentation、系统停止/切网及重开恢复；旧报告不作为新改动通过证据                                                      |
| A4                 | 导出与协程单测/设备用例、生命周期竞争；新增模块做独立依赖、Release/R8 消费和完整接入体积检查                                                     |
| 候选交付           | 受影响的 native 外部消费者、双 ABI AAR、外部 Release/R8 宿主、产物/许可/符号与严格发布检查                                                       |

执行要求：

1. 每次构建、启动模拟器、压力/大文件测试前读取实际可用物理内存；结合所选任务预算保留系统余量。
   资源不足时只回收本次启动且已完成工作的自有进程；仍不足则跳过并报告手动命令，不停止用户或其他任务进程。
   默认顺序执行高负载检查并限制构建并发，避免同时运行 native 构建、Gradle 和新模拟器。
2. 过滤 CTest 使用 `--no-tests=error`；Kotlin/JNI 用例核对实际类/案例数，更新驱动数量门槛而不是删除门槛。
   A1/A2 可复用 `run_m4_android.py`，A3/A4 按实际测试类选择设备入口。
3. 使用新的构建/证据输出路径，不覆盖既有 M0～M5 原始结果；记录提交/输入 SHA、命令、设备/API/ABI 和请求计数。
   服务器用合成签名/凭证，检查跨域去向和日志脱敏。
4. Linux sanitizer 与故障注入隔离构建。Windows、模拟器、SIGKILL、force-stop、重启和掉电分开表述；
   实际 Android >4 GiB 及物理设备门槛仍按发布检查表执行，不因本轮功能完成而豁免。
5. 每个阶段同步更新 API 声明、主机/Android 契约、sample 指南及受影响的 README；旧 M0～M5 证据保持原样。
   有行为变更但环境缺失时明确未验证项目，严格发布门槛保持阻断；未获发布指令不上传产物。

建议按可验证结果拆分提交，BUG 修复在描述中包含原因、修复和回归结果：

1. `test: 覆盖下载鉴权失败与新请求恢复场景`
2. `fix: 支持替换失效请求并保留检查点重试`
3. `feat: 支持类型化请求获取失败与可配置刷新状态`
4. `feat: 支持按持久标识和资源身份查询任务`
5. `feat: 完善 Android 多任务后台与网络执行协调`
6. `feat: 支持已下载资源流式导出`
7. `feat: 提供可选协程与任务快照适配`
8. `docs: 记录 Android 下载能力验收与接入边界`

## 9. 阶段完成清单

- [x] A0：状态转换、请求来源、预算和生命周期契约固定，主机失败复现通过；[结果与 Android 未执行项](../reports/android-request-recovery-a0.md)。
- [x] A1：新请求重试已实现，主机与 API 31 模拟器验证通过；[结果与剩余平台门槛](../reports/android-request-recovery-a1.md)。
- [x] A2：无订阅可直接查询持久化任务，[主机与 API 31 验证通过](../reports/android-task-query-a2.md)。
- [ ] 第一批交付：A1/A2 对应主机、Android、外部接入及文档检查有当前候选证据，缺测项明确。
- [ ] A3：多任务与网络协调不覆盖用户意图，系统停止后恢复经过明确许可。
- [ ] A4：导出与协程在取消/关闭竞争中无句柄丢失，线程、缓冲和事件保持有界。
- [ ] A5：各批次候选的设备矩阵、打包与体积证据可追溯，发布门槛按真实环境逐项判断。

返回 [文档中心](../README.md)。
