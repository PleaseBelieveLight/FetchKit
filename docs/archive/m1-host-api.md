# M1 主机暂存下载 API

[历史文档索引](README.md) · [当前主机 API](../reference/host-api.md)

历史阶段文档：对应 M1 提交 `5ed1e31`。后续 M2 的持久化、Completed、摘要和 lease 用法见
[M2 存储与恢复](m2-storage.md)；当前接入见 [主机 API](../reference/host-api.md)，下述能力仅属于 M1。

状态：alpha，2026-10-26。当前交付普通文件的单任务暂存下载、同进程暂停/恢复、续传协议与诊断查询。 Linux
是产品目标，Windows 用于当前开发验证；实测与缺测项见 [M1 报告](../reports/m1-baseline.md)。

## 能力与阶段边界

- 通过 `FETCHKIT_WITH_CURL=ON` 启用主机下载；普通 Android 构建保持关闭，不新增网络依赖，Kotlin 仍只有版本
  API。
- 每 engine 最多一个活动任务，额外入队返回 Busy，不暗中排队。同身份的尚存 task handle 返回
  Busy；多订阅/多任务共享和完整 Scheduler 留在 M3。
- 完成状态为 `FETCHKIT_TRANSFER_COMPLETE`，只说明消息结束、已知长度约束和暂存文件同步通过。未做可信
  SHA-256、未执行 Publishing/Completed、没有 acquire API。
- `staging_path` 仅用于诊断/测试。文件始终位于受管根目录的 `tmp/*.part`
  ；禁止将它当成已发布资源。进程结束后没有可恢复记录，不能按文件大小自行继续追加。
- Paused 表示此 engine 内的 I/O 已停止且数据同步完成，可根据内存中的偏移与校验器继续；M2 前没有持久化检查点保证。

## 构建和 CLI

已有 libcurl >=7.85 开发包时：

```powershell
cmake -S . -B build/m1-host -G Ninja -DCMAKE_CXX_COMPILER=g++ -DFETCHKIT_WITH_CURL=ON -DFETCHKIT_BUILD_TESTS=ON -DFETCHKIT_BUILD_PROBES=ON
cmake --build build/m1-host --parallel
.\build\m1-host\fetchkit_cli.exe download "https://example.com/file.bin" --root build/downloads --key model-v1 --max-bytes 8589934592
```

Windows 自定义 curl 安装位置通过 `CMAKE_PREFIX_PATH` 指定，动态库需要在 PATH。未启用后端时 create 返回
NotSupported；`--version` 不依赖 curl。

参考源码构建和 Linux 持续验证入口见 [依赖说明](../../third_party/README.md)：

```sh
bash tools/ci/linux_m1.sh
FETCHKIT_RUN_LARGE_FILE=1 bash tools/ci/linux_m1.sh
```

本地 HTTP 必须显式加 `--allow-http`。`--expected-length N` 提供预期长度；`--ca-bundle FILE` 提供可信
CA，不允许关闭证书校验。`--pause-after N` 在仍运行时演示同进程暂停再恢复，不能用于跨进程续传。

CLI 下载结束输出 JSON，进度写 stderr，最长每 200 ms 一次；未知长度只显示字节数。退出码：0 传输完成，2
参数/显式 HTTP 许可，3 网络/超时/证书，4 HTTP/鉴权，5 协议/长度，6 存储/容量，7 取消，8
Busy/后端不可用等本地失败。download 命令的 Ctrl+C 请求取消并清理暂存。

`shell --root DIR [--allow-http]` 保持 engine 存活，stdin 按行输入以下命令，stdout 返回逐行 JSON：

```text
enqueue "business-key" "https://example.com/file.bin"
query <返回的 task_id>
pause <task_id>
resume <task_id>
cancel <task_id>
release <task_id>
quit
```

这里 query 查询当前进程持有的 task；没有独立进程的持久任务查询。不要将真实 URL/token 放进共享终端记录或报告。SDK
错误和测试报告只使用错误类型/状态/布尔凭证标记，不打印获取地址或鉴权值。

## C ABI 与 C++ 包装

[fetchkit.h](../../include/fetchkit/fetchkit.h) 提供固定宽度整数、带长度 UTF-8 和非复用的 64 位句柄
token。 所有配置/请求/快照带 `struct_size`；先调用 init，输出快照先设置大小。输入在 API
调用期间借用并复制，快照完全归调用方。 字符串拒绝 NUL/非法 UTF-8/超限长度；受管根目录必须为 SDK
专用目录，不能混用同一个目录运行多个进程。

最小 C++ 调用：

```cpp
#include "fetchkit/fetchkit.hpp"

std::string root = "downloads", ns = "models", key = "model", version = "1";
std::string url = "https://example.com/model.bin";
fetchkit_engine_config config{};
fetchkit_engine_config_init(&config);
config.storage_root = fetchkit::stringView(root);
config.max_download_bytes = 8ULL * 1024 * 1024 * 1024;
fetchkit::Engine engine(config);
fetchkit_download_request request{};
fetchkit_download_request_init(&request);
request.resource_namespace = fetchkit::stringView(ns);
request.resource_key = fetchkit::stringView(key);
request.version = fetchkit::stringView(version);
request.url = fetchkit::stringView(url);
auto task = engine.enqueue(request);
auto snapshot = task.wait(1000); // 超时返回当时快照；继续等待或查询。
```

纯 C 调用和链接测试在 `tests/unit/c_api_test.c`。C/C++
同用一个状态模型：Queued、Running、Pausing、Paused、TransferComplete、Failed、Cancelled；`generation`
在显式恢复后递增。Failed 仅对 Network/Timeout 允许显式 resume，SDK 本阶段不自动做普通网络重试。

close/release 等待 worker、provider 和 I/O 停止。重复/过期 token 安全返回 Closed，关闭 engine 后 task
仍可读快照，不能再次运行。 release 正在运行的 task 会暂停并同步，不删除数据；显式 cancel 删除未完成数据。每
engine 最多保留 1024 个未释放的身份，释放后的索引在后续入队时清理。

M1 没有 attach/listTasks 或持久化记录；释放最后句柄、退出 shell 或关闭进程后，保留的 `.part`
没有续传入口，不能自行按其大小恢复。

## 宿主请求信息与鉴权

可只传 `address_reference` 和 provider，也可给初始 URL/headers 再由 provider 刷新。provider 在网络
worker 上同步调用，不持 SDK 锁；可重入 query、pause/cancel，阻塞式 wait、close/release 在该 worker 返回
Busy，避免自等待。

provider 的 context 包含 task/generation、Initial/Redirect/AuthRefresh、原 origin、提议 URL
和业务引用。context 在整个回调期间稳定；builder 只在回调内有效，set_url/set_header 立即复制。宿主数据必须存活到
task release 或 engine close 返回。

- callback 必须及时返回，长操作必须观察 `fetchkit_request_is_cancelled`。`provider_timeout_ms`
  在回调返回后检查超时， **不能强制中断任意宿主代码**，不承诺非协作回调的关闭延迟。
- 401/403 每轮最多刷新一次；失败返回 AuthRequired。回调抛出的 C++ 异常被转换，不传播到 worker/C ABI。
- 重定向跨 origin 清除所有继承的自定义头，再由宿主显式提供新凭证。provider 自行改变 URL 的 origin
  也会清除未在该次回调显式设置的头。
- 回调不能设置 Range、If-Range、Accept-Encoding、Host、Content-Length、Transfer-Encoding、Connection 或
  Proxy-Authorization。
- HTTP/HTTPS 以外的 scheme、URL userinfo、HTTPS 降级、循环和超预算重定向拒绝。参考构建为直连模式，不继承环境代理和代理凭证。

## 续传与错误

只凭强 ETag 发送 Range/If-Range；弱 ETag、缺失校验器、仅 Last-Modified 均先清空重下。 206 校验起点/终点/数字总长、强
ETag、旧总长/预期长度及实际响应字节数；当前保守策略要求 206 重复匹配的强 ETag，拒绝未知 `Content-Range`
总长。 合法局部 206 未到末尾时继续请求后段；忽略 Range 返回 200 先截断，不能追加。

416 或续传头不匹配最多额外完整重下一次；没有可信摘要/发布协议前， **不直接将 416 判为成功**。
编码、整数溢出、消息/长度冲突、超区间 body 等返回协议/完整性错误；磁盘、权限、写入短写与同步失败单独处理。
单任务字节上限计入已接收前缀，但不是 M3 的总空间配额；空间预留、LRU、pin/lease 尚未实现。

测试集合包括假传输状态机、真实 HTTP/HTTPS、CLI、同目录多进程和实际 >4 GiB；强制退出恢复与发布失败窗口仍属于
M2。
