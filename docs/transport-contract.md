# M0 传输契约

状态：2026-10-26，内部契约 v1；供 M1/M4 实现使用。`src/transport/http_transport.h` 和 Android
可选源码是验证此契约的实验接口，不是已发布下载
API。实测范围与缺口见 [M0 报告](reports/m0-baseline.md)。

M1 已将主机探针适配演进为 `CurlTransport`，并由 DownloadSession 控制响应接受、续传和重定向。
当前实现与限制见 [M1 API](m1-host-api.md)；Android 部分仍为可选实验，历史 M0 报告按原提交保留。

M4 已将隔离 OkHttp 5.5.1 产品化为 `AndroidTransport`，由 JNI 注入同一 C ABI engine 注册表， 不使用原
HttpURLConnection 候选。当前边界见 [Android API](m4-android-api.md)，历史探针继续保持可选。 正式适配显式声明
identity，绕过 OkHttp 内部状态重发，由 C++ 决定后续请求；复用 64 KiB ByteArray。
取消监视与连接/响应头截止时间不套用完整文件总时限；DNS 使用有界隔离 worker，明确保留系统解析无法强制中断的边界。

## 单次请求与策略归属

一次 operation 处理一个 HTTP/HTTPS GET。输入包含 URL、重复请求头、generation、连接超时、响应头/读空闲超时及显式
HTTP 许可。拒绝带 userinfo 的 URL、非法请求头、零/负超时和超限字段。请求由核心构造，宿主不能覆盖核心管理的
Range、If-Range 与 Accept-Encoding。

核心负责响应接受、重定向、鉴权刷新、重试和资源续传。后端不能自动跟随重定向、回复认证挑战、重试、使用响应缓存或透明解码；不得读取宿主全局
Cookie/Authenticator 而隐式发送凭证。代理也是显式配置项，不能继承未声明的代理凭证。M0
为保证可复现，两个实验后端均使用直连。

HTTPS 使用平台/后端正常证书与主机名验证；不提供“信任所有证书”开关。HTTP 默认关闭。测试明文例外仅放在示例
Debug Manifest，不进入 SDK 或示例 Release Manifest。

## 响应与接受屏障

响应头事件包含状态码、保留重复项的头列表、最终 URL、内容编码、generation 与可判定的消息长度信息。头名称按
ASCII 不区分大小写；不能将多条 Set-Cookie 等头粗暴拼接。限制总头字节与字段数量，解析整数时检查范围。

1. 跳过 1xx 中间响应；向核心交付当前请求的最终响应头。
2. 核心返回 Accept 或 Reject；等待决策时可以阻塞网络 worker，但不能占用核心控制线程。
3. 只有 Accept 后才能向 sink 交付响应体。Reject 必须终止 I/O，不交付任何 body 数据。
4. 3xx、401/403 等响应仍先交给核心；后端不自动创建后续请求。HTTP 状态本身不是传输失败，核心另行映射业务错误。
5. Content-Encoding 非 identity 时，首版核心拒绝；即使收到 gzip，也不能先解压后交付。
6. 尾部字段不能重新触发头接受回调或覆盖已接受的续传依据。协议异常与截断必须区别于正常 EOF。

M0 实验保留重复头、拒绝编码和 body 屏障；完整 Content-Range、206/416、重定向安全和续传判断在 M1
实现，不能以实验成功代替它们。

## 缓冲与背压

- 网络回调数据为只读借用，仅在回调返回前有效；核心不能保存裸指针。异步消费必须先复制到自有缓冲或显式移交所有权。
- Android 使用一个 64 KiB 可复用 ByteArray 批量跨 JNI；native 验证长度后同步复制至最多 64 KiB
  的局部缓冲，不保留 Java 引用。FNV-1a 仅用于比较传输内容，不能代替可信 SHA-256。
- M0 使用同步消费来验证背压，未实现双缓冲队列。正式实现预算为每任务最多两个 64 KiB
  数据块，排满后停止生产，取消唤醒所有等待。
- libcurl 可按其块大小交付，不能假设每次回调恰好 64 KiB；正式 adapter 必须保证单块不超过契约上限。
- 头屏障、背压和宿主请求信息获取都必须支持取消；不能只让 socket 读取可取消。

## 取消、结束与生命周期

operation 的网络资源只由其 worker 与受控的取消清理逻辑访问。取消请求幂等、可从控制线程发出。设置标记仅表示“已请求”，不是取消完成。

取消完成屏障要求：网络执行已返回，连接/输入流已清理，取消清理线程已结束，后端不再调用 sink
或访问借用缓冲。此后才允许释放 operation。结束原因仅产生一次：Complete、Cancelled、Timeout、Network、Rejected
或 ProtocolMismatch；原始平台错误作为附加字段。

正式异步接口必须将任务 ID + generation 绑定到每个事件。晚到的旧事件被控制队列丢弃，同时回收对应缓冲和
handle；不能把旧 Completed/Failed 应用到新一轮任务。M0 同步执行通过独立 sink 隔离
operation，尚未实现跨运行事件队列。

libcurl 探针使用 multi + 最多 50 ms poll，轮询取消并移除 easy handle；头/数据等待由 Cancellation
条件变量唤醒。它验证了本地阻塞连接，尚未证明任意同步 DNS resolver 都可取消。正式 Linux 构建必须记录
resolver 能力并覆盖解析/连接/TLS 阶段。

Android 探针将 disconnect/cancel 与流关闭置于独立清理线程，测量同时等待 worker 和清理线程结束；不假设
`close()` 总是立即返回。正常调用的异常消息不进入报告，仅记录类型，避免泄漏 URL/凭证。

## 固定参数与性能目标

| 项目         | 契约/起点                                                                               |
|--------------|-----------------------------------------------------------------------------------------|
| 头限制       | 64 KiB，最多 256 字段；更早阶段的后端内部分配仍需单独检查                               |
| 活动任务     | 默认 3；M3 才实现调度硬限制                                                             |
| 数据缓冲     | 64 KiB/块，正式队列最多 2 块/任务                                                       |
| 进度         | 最多每 200 ms，一般状态与终态不节流                                                     |
| M0 探针超时  | connect 5 秒，read/headers 2 秒；超时场景专用 250 ms                                    |
| 正式请求起点 | connect 5 秒、headers/read idle 15 秒；宿主请求信息获取 10 秒，可配置，M1/M4 实测后确认 |
| 取消目标     | 每场景至少 100 次，p95 ≤ 1 秒，单次 ≤ 3 秒；测量到真正静止，不含另行测量的检查点同步    |

M0 不为 DNS、外网 TLS、真机、其他 API 或磁盘检查点延迟提供未测保证。参数不是吞吐或弱网体验的最终调优结论。

## 后端适用性与参考资料

HttpURLConnection 必须通过全局 Cookie/Authenticator
场景才能被批准。探针特意保留真实平台行为；不能通过临时清空宿主全局处理器来让测试通过。替代方案应使用独立客户端并计入所有传递依赖，再以同一套场景复测。

libcurl
的等待与编码开关依据官方 [multi poll](https://curl.se/libcurl/c/curl_multi_poll.html)、[内容解码控制](https://curl.se/libcurl/c/CURLOPT_HTTP_CONTENT_DECODING.html)
文档。Android
全局认证语义见 [Authenticator](https://developer.android.com/reference/java/net/Authenticator)
。隔离客户端实验参考 [OkHttp 客户端源码](https://github.com/square/okhttp/blob/parent-5.5.1/okhttp/src/commonJvmAndroid/kotlin/okhttp3/OkHttpClient.kt)
和 [版本说明](https://github.com/square/okhttp/blob/parent-5.5.1/CHANGELOG.md)。
