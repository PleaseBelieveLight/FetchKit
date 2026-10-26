# M1 暂存下载与续传验证

日期：2026-10-26。基线提交：`3466326`，M1 实现位于其后的工作区；归档时的关键源码/构建输入哈希见
[参考构建记录](m1/reference-build.json)。M0 报告按原提交保留，不用 M1 源码覆盖历史证据。

结论：M1 的主机下载实现、alpha API、续传判断、CLI、固定 curl 源码配置与验证脚本已落地。 **当前实测在
Windows；缺少 Linux 运行记录，因此不标记 M1 整体验收完成。** Android 仅验证编译与普通构建兼容性，下载接入仍在
M4。

## 1. 工作包

| 工作包 | 交付                                                                 | 结果与边界                                                                       |
|--------|----------------------------------------------------------------------|----------------------------------------------------------------------------------|
| M1-01  | C ABI token/请求/错误/快照、C++ RAII、单活动任务、状态转换、provider | 纯 C 编译/链接测试及 9 个假传输会话通过；不是完整多订阅/多任务 API               |
| M1-02  | CurlTransport、64 位暂存文件、单目录锁、下载/同进程查询 CLI、退出码  | 正常、空文件、未知长度、截断、超时与取消有验证；不发布资源                       |
| M1-03  | 强 ETag/Range/If-Range、200/206/416、实际字节检查、一次安全重下      | 真实后端通过协议集合；弱/无校验器与 Last-Modified 安全重下                       |
| M1-04  | 核心手动重定向、origin 隔离、URL provider、一次鉴权刷新、可信 CA     | 本地 HTTP/HTTPS、降级拒绝、循环、凭证隔离和刷新检查通过                          |
| M1-05  | curl 源码 SHA-256 pin、许可证、Linux 验证入口、默认无 curl 回归      | 固定源码配置在 Windows 构建并运行通过；Linux 脚本仅做语法检查，未运行 Linux 验收 |

## 2. 可运行链路

`C/CLI → engine/task → DownloadSession → HttpTransport/CurlTransport → staging file`。 API
说明与示例见 [M1 主机 API](../m1-host-api.md)，测试入口见 [测试说明](../../tests/README.md)。

- 每 engine 一个 worker、一个活动任务，忙时返回 Busy；同身份只有一个存活 task handle，未实现订阅共享。
- API 输入复制，句柄通过 registry 校验并且不复用；控制和最终状态串行化，I/O 完全停止后才允许释放。
- 同进程暂停/恢复保留内存偏移与强校验器，暂停时同步文件；没有持久化记录，不支持跨进程恢复。
- 最终结果为 TransferComplete 暂存状态，路径保持 `tmp/*.part`，没有 acquire、DigestVerified 或已发布对象。
- 取消删除未完成数据，普通 handle release 停止并保留暂存；没有 attach/journal，最后句柄释放后的数据尚不能恢复。

## 3. 环境与依赖

| 项目              | 口径                                                                               |
|-------------------|------------------------------------------------------------------------------------|
| 主机              | Windows 11 x64，10.0.26200；g++/gcc 16.1.0，C++17                                  |
| 工具              | CMake 4.0.1、Ninja 1.13.2、Python 3.12.0                                           |
| 开发后端          | 已安装 libcurl 8.20.0；系统包配置，不能描述为项目锁定                              |
| 固定源码后端      | curl 8.20.0，官方归档与 SHA-256 在 `third_party/curl.lock.json`；只构建 HTTP/HTTPS |
| 参考 TLS/resolver | 实测元数据为 OpenSSL 3.6.4、AsynchDNS、Largefile；没有关闭证书/主机名验证          |
| Android           | 原有 AGP 9.4.1、Gradle 9.6.0、NDK 28.2.13676358、CMake 3.22.1；JDK 25.0.4          |
| 缺少环境          | WSL 查询未返回可用发行版，无 Docker/Podman；没有安装操作系统或伪造 Linux 测试结果  |

固定源码参考配置经过 SHA-256 校验后由 CMake 构建。协议保留 HTTP/HTTPS，启用 OpenSSL 与线程式
DNS，关闭压缩、PSL、SSH、外部 IDN、HTTP/2
等可选依赖；实际能力串在 [curl 契约摘要](m1/curl-contract-windows-pinned.json)。 OpenSSL、libc、编译器与系统
CA 仍来自宿主，未形成完整环境锁定，也不保证跨主机二进制相同。 上游许可证存于
`third_party/licenses/curl.txt`，源码包/预编译依赖不提交仓库。

Linux 入口为 `bash tools/ci/linux_m1.sh`；`FETCHKIT_RUN_LARGE_FILE=1` 显式注册并运行实际大文件测试。
该脚本不安装软件或改系统设置，缺少编译器/OpenSSL 开发包时明确失败。提供脚本不等于 CI 已在 Linux 通过。

## 4. 验证结果

| 集合               | 执行结果                                                                         |
|--------------------|----------------------------------------------------------------------------------|
| 固定源码常规 CTest | 6/6：服务器、纯 C ABI、假传输会话、真实协议、版本、curl 契约                     |
| 真实协议/CLI       | 55 项检查通过，原始结果见下方 JSON                                               |
| 假传输状态机       | 9 个会话通过，包括异常头拒绝、暂停/续传、200 重置、416 重下、恢复前无响应超时    |
| 服务器标准库测试   | 8 项通过，包括固定字节、重复头、Range、截断与凭证脱敏                            |
| M0 curl 契约回归   | 508 个样本通过，五类取消各 100 次                                                |
| 实际 >4 GiB        | 开发包与固定源码两种配置均完成实际传输和测试侧整文件 SHA-256 对照                |
| 无 curl 默认构建   | 构建及 4/4 单元集合通过，下载 create 明确返回 NotSupported                       |
| Android 普通配置   | SDK Debug/Release、sample Debug、sample 单测通过；本次未重跑设备 instrumentation |

协议覆盖：

- 普通/空/未知长度（chunked 与连接关闭定界）、无 Accept-Ranges、HEAD 不可用。
- 暂停后合法 206、连续局部 206、无强依据的重下、忽略 Range 返回 200、ETag/长度改变。
- 416（含本地字节已到总长的情况）受控重下；错起点/终点/总长、溢出、缺失区间/ETag、弱 206。
- 短 206、超区间 body、重复矛盾长度、长度溢出、identity 后强制 gzip、预期长度不符。
- 401/403 入口的 401 刷新路径与刷新次数上限、503 不被后端自动重试、网络中断后显式恢复。
- 相对/跨 origin 重定向、循环和次数上限、userinfo、HTTPS 降级拒绝、可信/不可信证书。
- callback 重入查询及阻塞自等待拒绝、协作取消 provider、关闭后恢复拒绝、过期 token 安全失败。
- CLI 下载退出码、shell 查询/暂停/恢复/释放及真实第二进程争用目录。

数据文件为 **4,295,032,835 字节**（4 GiB + 65,539），在网络中逐块发送、在磁盘中连续写入；不是稀疏文件或模拟计数。
测试侧逐块计算并比较 SHA-256：`228f3903446aeb88363ccbf444258f060ac18c5653d978b4bb2f5b04956c757c`。
大文件和测试 CA/私钥在验证后从受控临时目录清理，未提交二进制。 这项摘要由测试驱动执行， **不表示 SDK
已实现 SHA-256 校验**。

证据：

- [55 项协议结果](m1/protocol-windows-pinned.json)。
- [固定源码大文件结果](m1/large-file-windows-pinned.json)、[系统开发包大文件结果](m1/large-file-windows-system.json)。
- [508 项契约摘要](m1/curl-contract-windows-pinned.json)、[逐条样本](m1/curl-contract-windows-pinned.samples.jsonl)。
- [环境、源码/产物哈希与体积](m1/reference-build.json)。

## 5. 实现中明确处理的问题

- 不能把 206/416 当作“HTTP 成功即可追加/完成”。现在先判断强校验器、范围、总长及实际字节；异常头不交付
  body，416 保守重下，完整重下预算为一次。
- 只检查 Location 的 origin 会漏掉 provider 自行改变 URL 的情形。现在按实际 URL
  再判断，删除未在该次回调显式设置的继承头；provider context 的提议 URL 独立复制，setter 不使其失效，真实双
  origin 测试覆盖该路径。
- 暂停/取消与最后一块数据可能竞争。结束前冻结本轮控制，已经接受的取消可以清理数据，已进入最终处理的控制返回
  Busy；handle 释放等待 worker 停止，避免悬空 user_data。
- 新一轮开始前清空旧 HTTP 状态，防止连接/响应头超时带上上一轮的 200；新增假传输用例覆盖。显式头名追踪去重并受
  64 项限制，避免重复 setter 导致无界增长。

## 6. 包体积与范围限制

普通双 ABI SDK Release AAR 仍为 **140,418 字节**，SHA-256 与 M0 一致。 其中 arm64 JNI 为 208,736
字节，x86_64 JNI 为 208,152 字节；无 curl/SSL 新库、无实验类。 原因是 Android 仍只调用独立版本对象，新增下载核心对象未被链接进
JNI。不能据此宣称完整 Android 下载 SDK 的增量为零；M4 接入后必须重新量化。

主机 curl 后端是显式选择的依赖。固定源码的 curl 静态库/CLI Debug 文件及完整依赖口径在构建记录中单列；Debug
文件和带符号静态归档不作为最终发布体积。Linux 发布时的 TLS/STL/动态库、优化配置和许可证打包仍由 M5 实测。

当前未完成且不能推导的能力：Linux 实际执行、真机网络、DNS/连接/TLS 阶段取消延迟、可控
ENOSPC/短写注入、可信摘要、持久化记录、强制退出恢复、原子发布、资源租约、总配额、多任务调度与后台执行。
provider 为协作式同步回调，SDK 无法强制终止任意宿主代码；超时在返回后检查，阻塞 provider 会影响关闭延迟。

M2 应沿用现有头接受和偏移约束，增加记录与发布协议；先补 Linux 证据，再对外承诺该平台的 M1 验收通过。
