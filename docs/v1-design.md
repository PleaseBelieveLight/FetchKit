# FetchKit 首版设计基线

日期：2026-10-23。状态：工程骨架已建立，下载、续传和缓存能力待实现。

2026-10-27 M4 进展：异步 Kotlin/JNI、隔离 OkHttp 5.5.1 后端、任务/资源示例与 JobScheduler 适配已加入。
见 [Android API](m4-android-api.md) 和 [M4 报告](reports/m4-baseline.md)。核心策略仍由 C++ 决定；
后台示例在 API 34+ 使用 UIDT，低版本使用有限的普通任务，显式绑定获准 Network，不强制依赖 WorkManager。
API/ABI 真机矩阵、实际 >4 GiB、Linux/sanitizer 和重启/掉电仍须分别提供证据。

2026-10-27 M3 进展：多任务、独立订阅、持久化重试、配额/LRU/pin/过期及读取保护已有主机实现，
见 [M3 API](m3-host-api.md)、[v2 记录](storage-record-v2.md) 和 [报告](reports/m3-baseline.md)。
宿主在自己的 executor 上主动分发有界事件；Android 正式接入仍待 M4，Linux/sanitizer 等缺测不由 Windows
代替。

2026-10-27 M2 进展：已增加检查点、跨进程重新绑定、可信 SHA-256、发布恢复与最小只读租约。
当前实现与同步边界见 [M2 存储](m2-storage.md)、[记录格式](storage-record-v1.md)
和 [报告](reports/m2-baseline.md)。 Linux/重启/掉电尚无执行证据；完整阶段门槛仍受这些缺测项约束。以下保留原始设计和阶段目标。

2026-10-26 M1 进展：主机暂存下载与续传校验已有实现，详见 [M1 API](m1-host-api.md) 和
[阶段报告](reports/m1-baseline.md)。Android 下载、持久化检查点、摘要验证、资源发布和缓存仍待后续里程碑；
Linux 尚未实测，不能将 M1 标为整体验收通过。

当前结构以根目录 Gradle 工程管理 `:sdk`（`platform/android/sdk`）和 `:sample`（
`platform/android/sample`），根目录 CMake 单独构建 C++ 核心及主机 CLI。已打通版本查询链路，本文的下载能力和里程碑仍是实现目标。

M0 的可执行探针与阶段证据见 [M0 基线报告](reports/m0-baseline.md)，细化契约见
[传输契约](transport-contract.md) 和 [API/存储契约](api-contract.md)。探针不等于正式下载
API；阶段缺测项仍须补齐。

## 1. 产品定位与边界

FetchKit 定位为可扩展的 C++ 网络资源下载 SDK。首版提供“可靠的 HTTP/HTTPS 文件下载 +
业务资源存储”。调用方提交资源身份、获取地址及可选的可信摘要，SDK 负责下载、恢复、校验、发布和空间管理。

文件内容按字节处理，因此文件扩展名不决定支持范围。MP4、MP3、图片、ZIP、地图包、模型和游戏资源包可以共用一套实现。压缩包在这里作为完整文件下载，解压与安装由调用方处理。

“覆盖多数场景”指常见 HTTP 文件交付方式，不能据此承诺覆盖所有视频离线、下载协议或服务器行为。

| 范围       | 首版决策                                                                          |
|------------|-----------------------------------------------------------------------------------|
| 平台       | Android 接入 + Linux 参考实现；C++17 + CMake                                      |
| 下载方式   | GET；每个文件一个连续写入位置；允许多个文件并发                                   |
| 文件大小   | 使用 64 位偏移；支持超过 4 GiB 的文件；处理长度未知和空文件                       |
| 任务控制   | 入队、暂停、恢复、取消、查询、启动时恢复持久化任务                                |
| 网络       | HTTPS 优先，HTTP 显式配置且受平台策略约束；超时、有限重定向、请求头、鉴权刷新入口 |
| 续传       | 单区间 Range；资源变化或不支持续传时安全重下                                      |
| 完整性     | 已知长度检查；可选可信 SHA-256；明确记录是否通过摘要校验                          |
| 存储       | 暂存、原子发布、空间配额、LRU、固定保留、读取租约                                 |
| Android    | Kotlin API、JNI、平台网络后端、后台执行接入示例                                   |
| 观察与演示 | 结构化错误、进度、脱敏诊断、Linux CLI、可控故障服务器                             |
| 后续范围   | 多连接分段、HLS/DASH 编排、DRM、P2P、FTP、差分更新、解压安装、边下边播            |

不完整文件不会通过资源读取 API 暴露。暂停后可以再次启动；进程退出后由宿主重新启动 SDK
和恢复任务，不能承诺进程持续存活。

首版同一存储目录采用单进程所有权锁。第二个进程打开该目录时返回 Busy，避免共享写入造成损坏。

## 2. 架构与依赖

项目根目录、Git 仓库、CMake 工程名和 C++ 命名空间统一为 `fetchkit`，展示名称为 FetchKit。公共头文件放在
`include/fetchkit/`，命令行目标使用 `fetchkit_cli`，项目构建选项使用 `FETCHKIT_` 前缀。

Android SDK 的 namespace 和 Kotlin 包名固定为 `com.luofelix.fetchkit`。演示 App 的 namespace 为
`com.luofelix.fetchkit.sample`，applicationId 保留为 `com.luofelix.fetchkit`。JNI 使用
`RegisterNatives` 绑定 SDK 类，并通过 consumer rules 保留所需名称。

```text
Kotlin API / Linux CLI / C++ 调用方
                 |
           SDK 公共 API
                 |
        TaskManager + Scheduler
                 |
       DownloadSession（续传决策）
          /                  \
    HttpTransport          ResourceStore
    /          \           /          \
Android      libcurl   TaskJournal   CacheManager
平台网络     Linux      检查点记录    配额与读取租约
                 |
        FileSystem / Verifier / Clock
```

任务状态、续传判断、文件写入、发布和缓存策略归 C++ 核心。Android 平台层负责网络 I/O、系统调度、网络约束和通知。

### 2.1 网络后端

M0 后 Android 默认实现方向调整为通过 JNI 接入独立配置的 OkHttp 客户端，Linux 采用 libcurl。 Android
仍复用平台 TLS 和证书能力，但必须计入 OkHttp、Okio、Kotlin 标准库及其他传递依赖的实际接入成本。
当前仅交付可选实验，正式适配仍在 M4 完成，版本和体积证据见 [M0 报告](reports/m0-baseline.md)。

最初候选 `HttpURLConnection` / `HttpsURLConnection` 在 API 31 探针中会使用宿主全局 CookieHandler 和
Authenticator，绕过核心凭证与鉴权策略，因此不能直接作为默认后端。不能通过临时替换宿主全局处理器规避该问题。
独立客户端必须显式关闭自动重试、重定向、Cookie、Authenticator 和缓存，并使用 identity 编码；M1/M4
继续验证完整协议。

Android 中需要统一 native 网络栈的集成方，可以选择后续适配的 libcurl 构建配置。首版不要求交付 Android
的两个后端。

`HttpTransport` 契约必须包含：

- 先交付最终响应头，核心明确接受后才能向文件写入响应体。
- 提供状态码、重复响应头、最终 URL、编码和可判定的传输结束状态。
- 请求、取消和资源释放具有明确的线程与生命周期规则；过期任务的回调通过 generation 标识丢弃。
- 流式交付数据并支持背压，避免将整个文件读入内存。
- 核心控制续传请求头、重定向和鉴权重试；后端自动重试不得改变这些语义。
- 正确处理 HTTPS、连接错误、读超时、协议截断，并映射为统一错误。
- 禁用后端响应缓存，避免与资源存储重复管理。

两个后端共用协议测试。Android 使用可复用缓冲区批量交付数据，避免按字节跨 JNI；缓冲区所有权和回调返回后的有效期必须写入契约。

M0 必须实测“阻塞读取时取消”的行为。仅设置取消标记不能视为完成取消；关闭输入流、连接和读取超时应共同提供可测量的取消延迟。

### 2.2 API 和线程

- 预编译边界采用小型 C ABI 和 opaque handle；C++ 提供 RAII 包装，Kotlin 提供类型化接口。
- 字符串、缓冲区和错误对象的所有权明确；异常不能跨 C ABI 或 JNI 边界。
- 公共 API 可从宿主线程调用，命令进入核心控制队列；同一任务的状态转换串行执行。
- 首版使用有界工作线程和缓冲区，默认最多 3 个活动下载；参数作为调优起点。
- 同一资源身份只保留一个写入者，多订阅者共享结果；所有订阅者都退出后才停止共享任务。
- 用户回调经指定 executor 分发，调用时不持有内部锁；单任务事件有序，终态回调在一次运行中最多一次。
- 进度通知默认最多每 200 ms 一次；长度未知时不伪造百分比。

建议公共能力：`enqueue`、`pause`、`resume`、`cancel`、`query`、`listTasks`、`acquire`、`pin`、`unpin`、
`remove`、`trimTo`。

`pause` 请求发出后，应等到进入 Paused 才认为检查点完成。取消默认清理未完成数据；已完成资源由显式删除
API 管理。

M1 alpha 接口仅提供单个活动任务、同进程暂停/恢复和 TransferComplete 暂存结果，未交付发布/读取 API。 此阶段
Paused 只确认数据文件同步与内存偏移，不能承诺跨进程恢复。M2 才形成完整持久化检查点； 多订阅、回调
executor、总配额与缓存生命周期已由 M3 主机实现补齐，详见 [M3 API](m3-host-api.md)。 缓存策略放在
`src/storage/storage_policy.cpp`，与记录变更、删除和租约共用事务及保护锁。

M0 将共享控制细化为：pause/resume 对共享 task 生效；cancel 只撤销指定订阅，最后一个订阅显式取消时才取消共享下载。
普通订阅释放在最后一个订阅退出时停止 I/O、尽力提交并保留暂停任务；engine 关闭同样保留恢复信息。
恢复任务需由宿主重新订阅并给予执行许可，不复活旧回调。读取租约可保留最小存储上下文及目录锁至其释放。

建议错误类型：Network、Timeout、HttpStatus、AuthRequired、ProtocolMismatch、IntegrityMismatch、DiskFull、PermissionDenied、CacheQuotaExceeded、Busy、Cancelled。原始
HTTP 状态和平台错误作为附加诊断信息保留。

M0 另细化 InvalidArgument、NotFound、Closed、UnsupportedMetadata 和 SubscriptionOverflow 等本地错误，
明确共享控制、事件容量及租约关闭行为，见 [API 契约](api-contract.md)。这些类型尚未形成已发布 API。

## 3. 资源身份与请求信息

持久化任务至少包含：task_id、namespace、resource_key、version、representation_key、获取地址引用、预期长度、预期摘要、校验器、已提交偏移、状态和元数据版本。

资源身份由调用方的业务 key、版本、账户命名空间及内容变体共同确定。变体包括会改变响应内容的请求参数或请求头；同一个
URL 不一定代表同一份资源。

- namespace 隔离不同账号和业务域，避免私有资源串用。
- 签名 URL 和 token 可以刷新，但刷新不能悄悄改变资源身份；仍需通过强校验器或可信摘要确认内容。
- 鉴权头由宿主在发起请求时提供，默认不持久化；签名 URL 默认只保存可重新获取它的业务引用。公共 URL
  可由宿主显式允许持久化。
- 日志不记录 token、Cookie、完整签名 URL 或完整鉴权头。
- 文件名由内部身份映射生成，远端文件名只作展示元数据。

缓存是显式业务资源存储，首版由业务版本和可选过期时间控制复用；完整 HTTP 缓存的新鲜度和协商语义属于另一个范围。需要透明
HTTP 缓存的接入方应另行评估。

## 4. 续传规则

下表是基于 [RFC 9110 的 Range 与条件请求规则](https://www.rfc-editor.org/rfc/rfc9110.html#section-14)
制定的保守实现策略。服务端可以忽略 Range；`Accept-Ranges` 和 HEAD 只能提供线索，不能替代对实际 GET
响应的校验。

| 情形               | SDK 策略                                                 |
|--------------------|----------------------------------------------------------|
| 具备强 ETag        | 请求 `Range: bytes=N-` 和对应 `If-Range`                 |
| 只有弱 ETag        | 不用于 If-Range                                          |
| 只有 Last-Modified | 首版默认不凭它续传；日期满足强校验条件后的支持可单独实现 |
| 缺少可信续传依据   | 默认从头重下；后续可开放以最终可信摘要兜底的策略         |
| 返回 206           | 校验起点、区间、总长与已有校验器一致，再接收数据         |
| 返回 200           | 按新下载处理，先重置暂存文件和元数据，绝不追加           |
| 返回 416           | 重新确认版本、总长与本地完整性；不能直接判成功           |
| 区间错误或版本冲突 | 拒绝追加，受控重下或返回协议错误                         |

`If-Range` 不能使用弱
ETag，日期也有强校验条件，见 [RFC 9110 §13.1.5](https://www.rfc-editor.org/rfc/rfc9110.html#section-13.1.5)。

续传期间使用 `Accept-Encoding: identity`，关闭透明内容解码；意外收到非 identity
内容编码时，首版返回可诊断错误。其余内容类型仍作为原始文件下载。Android 平台默认 gzip
行为需要显式控制，见 [HttpURLConnection 编码说明](https://developer.android.com/reference/java/net/HttpURLConnection)
；libcurl
的对应行为见 [CURLOPT_ACCEPT_ENCODING](https://curl.se/libcurl/c/CURLOPT_ACCEPT_ENCODING.html)。

核心还要检查响应区间的实际字节数。一次合法的局部响应未覆盖文件末尾时，可以继续请求后续区间；不能将每个成功的
206 都视为整个文件完成。

长度未知时，按网络后端确认的正常消息结束处理；若调用方没有提供预期长度或可信摘要，就无法证明响应内容符合发布方预期。

重定向必须有限次且有循环检测；默认禁止 HTTPS 降级，跨 origin 时不自动转发鉴权头，凭证是否重新提供由宿主决定。

## 5. 状态、检查点与发布

主流程：Queued → Running → Verifying → Publishing → Completed。

Running 可进入 Pausing → Paused，或 RetryWaiting；活动状态可进入 Cancelled / Failed。进程退出时持久化的
Running 恢复为可恢复的待调度状态，是否启动取决于宿主策略。

每份资源采用暂存数据文件与独立元数据记录。记录包含格式版本、有限长度字段和记录校验值；写入通过临时文件和同目录原子替换进行。首版采用可扫描的独立记录，避免强制引入数据库，任务数量较大时再评估索引。

M0 将编码方向细化为有界二进制 TLV，记录使用 CRC32 检测损坏，字段上限和版本隔离见 API 契约。 M2
已固定 [字节格式与黄金样本](storage-record-v1.md)，CRC 使用平台 zlib，内容 SHA-256 使用固定提交的 MIT
许可 PicoSHA2。 记录校验不能代替可信内容摘要；Windows 同步边界与缺测项单列。

检查点写入顺序：

1. 写入暂存文件，处理短写和磁盘错误。
2. 同步已经写入的数据。
3. 原子更新 committed_offset 及相关元数据，并执行平台支持的必要同步。
4. 恢复时只信任已提交前缀；文件长于偏移则截断，短于偏移或记录损坏则进入保守恢复。

不将内存中的 received_bytes 或当前文件大小直接等同于可恢复偏移。检查点频率同时受时间和累计字节数控制，避免每个数据块都同步磁盘。

完成时先确认消息结束与已知长度，再执行要求的摘要校验，随后同步数据、进入 Publishing，并在同一文件系统内
rename 到最终对象路径，最后持久化 Completed。系统支持时同步父目录。

两个文件不能靠两次 rename 获得整体事务性。启动扫描必须处理“文件已发布、元数据仍停留在
Publishing”等中间状态：重新确认长度和所需摘要后补全记录，或清理异常对象。只有一致的 Completed 对象能被
acquire。

默认资源路径位于 SDK 管理的同一文件系统。导出到其他路径或 Android 文档 URI
时另行复制并确认结果，不能把跨文件系统移动描述为原子操作。

进程强制退出是首版强制测试项。重启和掉电的保证取决于文件系统及同步实现，必须单列实测，不能仅用 kill
测试宣称已经覆盖。

## 6. 完整性与缓存生命周期

两种完成结果需要对调用方可见：

- TransportComplete：后端正常结束，已知长度等约束通过，但未对照可信摘要验证。
- DigestVerified：额外对照调用方提供的可信 SHA-256 验证通过。

调用方可设置 require_digest，缺少摘要时拒绝创建严格校验任务。ETag 用作版本校验器，不能泛化为文件摘要；下载后自行计算的
hash 也不能证明内容与发布方一致。

校验器采用独立接口。SHA-256 选择经过验证且许可证合适的实现或可复用的平台能力，不自创算法，也不只为摘要强制引入完整
TLS 库。续传后可重新读取文件计算最终摘要，首版不持久化哈希内部状态。

摘要不符的文件不发布。完整性错误独立记录，默认不自动反复重试；如配置一次完整重下，必须丢弃此前暂存内容。

缓存规则：

- 总配额计入最终文件、暂存文件及受控的元数据开销；并发写入统一记账。
- 已知长度先预留空间；未知长度按块申请额度，并受单任务最大字节数限制。
- 空间预检不能替代写入错误处理，运行中仍可能出现 ENOSPC。
- acquire 返回有生命周期的读取租约；持有租约的对象不能被淘汰。
- pin 表示业务要求保留，适用于用户主动离线下载；普通缓存按 LRU 淘汰。
- 所有空间都被 pin 或使用中时返回明确的配额错误，不强删正在使用的资源。
- 长时间遗留的暂存文件按独立策略清理，不能只统计 Completed 文件。
- Android 中普通缓存可放 cacheDir；需要长期保留的离线资源使用 filesDir 或明确选择的存储位置。pin 仅约束
  SDK 自己的淘汰行为。

摘要校验保证发布时的结果。若产品还要求检测发布后的磁盘损坏，需要配置读取前复验或周期检查，不能靠文件大小和修改时间推导内容正确。

## 7. 重试与 Android 后台执行

可重试错误采用指数退避、随机抖动和最大尝试次数，并处理合法的 Retry-After。默认重试对象包括瞬时网络故障、超时、408、429
和选定的 5xx；证书、协议、权限、磁盘及摘要错误分别处理。

401 / 403 通过宿主鉴权刷新入口处理，限制刷新次数；刷新失败后返回 AuthRequired。用户暂停、取消和系统停止不会被普通网络重试覆盖。

Android 的系统执行许可与 SDK 的下载策略分层：

- SDK 持久化下载身份、进度和失败状态。
- 宿主根据当前平台规则选择 WorkManager、用户发起的数据传输任务或合适的前台执行方式，并负责通知与网络约束。
- 系统要求停止时取消 I/O 并尽力提交检查点，下一次获得执行机会时恢复。
- 将普通 JNI 包装与可选调度适配分成不同模块，避免所有接入方都必须引入 WorkManager。

具体选择取决于是否用户发起、是否需要立即开始和进度通知，见 [Android 后台数据传输选择](https://developer.android.com/develop/background-work/background-tasks/data-transfer-options)
。首版提供示例和接入说明；不承诺规避系统限制，也不以 JNI 替代系统调度。

## 8. 体积控制和度量

先测量，再制定数字预算。发布报告分开列出：

| 指标         | 测量口径                                                       |
|--------------|----------------------------------------------------------------|
| 核心本身     | 单 ABI Release、strip 后的核心产物；记录编译器和开关           |
| 接入总增量   | 同一空壳 App 接入前后 APK 的实际差值，包含新增运行库与传递依赖 |
| 分发体积     | AAR 全部 ABI 体积，以及设备实际交付的单 ABI 安装产物           |
| 网络配置差异 | Android 平台后端与独立打包网络栈的依赖成本；如未实现则明确缺测 |

实现措施：

1. 核心、JNI、网络后端、调度适配和示例分目标构建，可按需链接。
2. 评估 `-Os` / `-Oz`、LTO、函数与数据分段、无用代码删除，并对性能做必要回归。
3.

限制动态导出符号，保留调试符号的独立归档；参考 [NDK 符号可见性](https://developer.android.com/ndk/guides/symbol-visibility)。

4. 控制公共头文件模板和强制依赖；日志、CLI、测试工具独立交付。
5. libcurl
   构建仅启用业务所需协议和能力；裁剪依据见 [curl 构建文档](https://curl.se/docs/install.html#reducing-size)。
6. 统一宿主与 SDK 的 C++ runtime
   策略，计入新增运行库；参考 [NDK C++ runtime 约束](https://developer.android.com/ndk/guides/cpp-support)。
7. 首版测量 arm64-v8a 发布配置，x86_64 用于模拟器验证；其他 ABI 按集成需求构建。

不在实测前写入“几百 KB”一类承诺；不能仅报告排除了 TLS、STL 或调度依赖的核心文件大小来代表接入成本。

## 9. 里程碑与验收

| 阶段             | 交付物                                               | 完成依据                                            |
|------------------|------------------------------------------------------|-----------------------------------------------------|
| M0：关键假设     | 后端契约、小型网络探针、Android 体积基线             | 编码、重定向、取消延迟和依赖成本有测量结果          |
| M1：续传链路     | Linux 单文件下载、libcurl 适配、可控 HTTP 测试服务器 | 200 / 206 / 416、资源变更和错误区间处理正确         |
| M2：恢复与校验   | 检查点、发布协议、SHA-256、启动扫描                  | 强制退出和发布中间状态恢复；损坏文件不暴露          |
| M3：任务与缓存   | 队列、多任务、重试、租约、pin、配额                  | 同 key 竞争和空间不足测试通过，内存随并发上限有界   |
| M4：Android 接入 | JNI、Kotlin、平台网络适配、可选后台示例              | 与 Linux 共用协议用例，真机生命周期和大文件验证通过 |
| M5：首版发布     | CLI、AAR、接入示例、版本与错误文档、体积报告         | 故障测试清单通过，发布包内容与报告一致              |

M5 已提供 [本地交付预览和发布流程](m5-release.md)、[检查表](release-checklist.md) 与
[实际证据](reports/m5-baseline.md)。版本统一在根目录 VERSION，CMake 导出采用精确版本匹配，AAR 通过本地
Maven 保留传递依赖。项目自身采用 Apache-2.0，第三方许可独立保留。当前仍为
alpha；Linux/物理设备矩阵和完整许可审核等门槛未满足时，发布检查保持阻断。

首版必须覆盖的自动化或设备测试：

- 正常下载、空文件、长度未知、超过 4 GiB，验证内容与偏移。
- 无 Accept-Ranges、HEAD 不可用、合法局部响应、忽略 Range 返回 200。
- 416、错误 Content-Range、前后 ETag 或长度变化、弱 ETag。
- 无强校验器时安全重下；声明 identity 后仍收到其他内容编码。
- 重定向循环、跨 origin 的凭证处理、过期地址和鉴权刷新。
- 断网、读超时、429 / 503 及 Retry-After；取消与重试竞争。
- 下载、检查点、校验、rename、元数据提交各阶段强制退出，再启动恢复。
- 暂存数据短于或长于 committed_offset；元数据截断、损坏和版本不兼容。
- 内容摘要不符、预期长度不符、已发布对象丢失；不返回坏对象。
- 写入短写、磁盘写满、配额耗尽、未知长度超限、目录权限失败。
- 多任务竞争配额、同 key 重复请求、读取中淘汰、pin 阻止回收。
- 第二进程争用目录、回调内调用控制 API、关闭 SDK 后迟到回调。
- Android 进程终止、系统停止任务、网络切换、不同 API 级别的后台策略。
- 目标设备的系统重启与存储恢复测试，结果单列，不与进程终止测试混写。

Linux 故障测试首先使用本机可控服务器，固定响应头、字节内容、断开位置和延时，避免依赖公共网站的偶然行为。

建议后续目录：`include/fetchkit/`、`src/core/`、`src/storage/`、`src/cache/`、`src/transport/`、
`platform/android/`、`examples/cli/`、`tests/`、`tools/size/`。首版落地按里程碑逐步建立目录，避免先创建大量空模块。
