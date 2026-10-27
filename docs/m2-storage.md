# M2 持久化、校验与发布

状态：alpha，2026-10-27。替代 M1 的仅内存暂存结果。当前仍为每 engine 一个活动下载，Android Kotlin/JNI 下载接口留在 M4。
实测与缺测项见 [M2 报告](reports/m2-baseline.md)。C 结构已扩展，使用方必须重新编译；尚不承诺发布版 ABI 稳定性。

## 下载与恢复

```powershell
cmake -S . -B build/m2-host -G Ninja -DCMAKE_CXX_COMPILER=g++ -DFETCHKIT_WITH_CURL=ON -DFETCHKIT_BUILD_TESTS=ON -DFETCHKIT_BUILD_PROBES=ON -DFETCHKIT_ENABLE_FAULT_INJECTION=ON
cmake --build build/m2-host --parallel
ctest --test-dir build/m2-host --output-on-failure -LE large_file
.\build\m2-host\fetchkit_cli.exe download "https://example.com/model.bin" --root build/downloads --key model-v1 --sha256 <可信的64位十六进制摘要> --require-digest
```

系统 curl/zlib 的开发文件不在标准位置时设置 `CMAKE_PREFIX_PATH`；固定 curl 参考配置见
[依赖说明](../third_party/README.md)。不在普通 SDK 构建中开启故障注入。

- 创建 engine 获取目录所有权锁并同步扫描，恢复记录默认不会发起网络请求。扫描可能读取完整文件，不能放在 UI 主线程。
- `fetchkit_list_tasks` 按持久 ID 排序分页，容量为 1～100，返回已提交的持久状态及身份/业务引用；不是实时进度列表。
- `fetchkit_attach` 使用原身份、长度/摘要/require_digest 和业务引用绑定，重新复制本次 URL、headers/provider，保持暂停。随后显式 `resume` 才执行。
- 重新 `enqueue` 同一身份也会绑定已有记录；它明确允许执行可恢复任务。Completed 直接返回已有结果；不可恢复的 Failed/Cancelled 保持终态。
- 相同身份的约束或业务引用冲突返回 InvalidArgument；同身份仍有句柄时返回 Busy，独立订阅共享属于 M3。
- URL、headers、provider、用户指针、旧 task token 均不落盘。只保存宿主给定的业务引用；引用本身必须是不含凭证的标识。
- task token 只对当前进程有效；`persistent_id` 是身份四元组以 NUL 分隔后计算的 SHA-256，跨进程稳定。身份字段拒绝 NUL。
- attach 不改变 generation；显式恢复或重新入队运行时递增。关闭/最后句柄释放尽力暂停并提交，错误保留在 snapshot，Paused 才确认检查点成功。

CLI `download` 重复使用同一个 `--root` 和 `--key` 即为显式恢复；摘要和长度约束必须与原任务相同。
`shell` 新增 `list [offset]` 和 `attach "key" "url"`，其余 query/pause/resume/cancel/release/quit 保持原用法。
shell 创建的任务当前不接收摘要参数；严格任务使用 C/C++ API 或 download 命令。不要在共享终端记录真实签名 URL。

## 检查点与安全重下

根目录布局为 `tmp/<id>.part`、`records/<id>.rec`、`objects/<id>.obj` 和 `quarantine/`。
不信任远端文件名，记录不含任意文件路径。目录及文件叶节点拒绝符号链接；根目录须由 SDK 独占管理。

`received_bytes` 是本轮接收进度；`committed_offset` 只在以下全部成功后前进：

1. 完成文件写入，循环处理短写和 EINTR。
2. 同步数据（POSIX fsync / Windows FlushFileBuffers）。
3. 写同目录 `.next` 记录，同步文件，原子替换 `.rec`，执行支持的目录同步。

默认每新增 4 MiB 或距离上次提交 1 秒，在下一个数据回调触发；暂停、网络终止、完成单独提交。
无数据流动时不启动仅为检查点服务的定时线程。配置可调整正数阈值，时间上限 300000 ms。
同步/提交失败不会把接收偏移报告成已提交偏移，任务进入存储错误。

启动只信任记录前缀。文件更长则截断到 committed；更短或丢失则先提交零偏移并清除 ETag，再保守重下。
无强 ETag 的已提交前缀仍从头重下；强 ETag、206/416 的严格规则沿用 M1。
安全重下先提交零偏移，随后截断，避免退出窗口将新内容与旧校验器关联。不会通过文件大小猜测续传点。

## 校验、发布和只读租约

正常结束和已知长度通过后执行 `Verifying → Publishing → Completed`。`FETCHKIT_TRANSFER_COMPLETE` 的旧状态值保留，
M2 不再以它作为终态；完成类型使用独立的 `snapshot.integrity`：

| 值 | 含义 |
|----|------|
| `FETCHKIT_INTEGRITY_NONE` | 尚未发布或验证失败 |
| `FETCHKIT_TRANSPORT_COMPLETE` | 传输正常结束、长度约束通过，未对照可信内容摘要 |
| `FETCHKIT_DIGEST_VERIFIED` | 另外对照宿主可信 SHA-256 通过 |

`expected_sha256` 为空或 64 位十六进制，统一转小写；`require_digest=1` 且无摘要在入队前返回 InvalidArgument。
使用固定提交的 MIT 许可 PicoSHA2；校验以 64 KiB 块重新读取完整文件，不持久化哈希内部状态，不为摘要新增 TLS 库。
摘要不符是 IntegrityMismatch，保留失败诊断、不发布、不自动重试。ETag 和 CRC 不能替代可信摘要。

发布先持久化 Publishing（包含完成长度与校验结果），同文件系统 rename，再同步两个父目录，最后持久化 Completed。
进入 Publishing 前串行决定暂停/取消；此后控制返回 Busy，避免已发布对象被取消清理。校验期间仍可协作暂停/取消。
启动遇到 Publishing，会对候选文件重新检查长度和所需摘要，再完成 rename/Completed；缺失或损坏则 Failed，拒绝 acquire。
Completed 启动时也检查文件；坏对象移入隔离目录，无法通过读取 API 取得。

`fetchkit_resource_acquire` 只接受一致的 Completed ID，并在有摘要时复验。返回 opaque 只读 lease；
`fetchkit_resource_read(lease, offset, buffer, capacity, &count)` 按偏移读取，最多返回 64 KiB，EOF 返回 count=0。
没有写入或暂存路径读取入口。租约可跨 engine close 存活，并保持文件和目录锁；最后租约释放后才允许同目录新 engine。
C++ `Engine::acquire` 返回可移动 RAII `ResourceLease`。调用方不得直接修改 SDK 对象；没有摘要时无法检测同长度内容损坏，
已有租约也不承诺检测外部对文件的事后篡改。

## 恢复与隔离边界

记录损坏/未来版本逐条移入 quarantine 并统计，原始字节保留，其他任务仍可打开。未知/孤立文件只统计并保留，M3 再实施有配额和保留期的回收。
原 M1 的数字文件名 `.part` 没有可信记录，按孤立文件保留，不迁移为可恢复资源。无记录但同 ID 文件已存在时拒绝覆盖。
隔离记录的 ID 也保留禁止覆盖标记；即使它的数据文件已丢失，旧实现仍不会为该身份自动重建记录。
`fetchkit_engine_storage_stats` 返回本次扫描发现的隔离数、未知版本数、孤立文件数与记录数；自动重试、LRU、pin、过期和 remove 均留在 M3。

POSIX 同步目录失败会显式报错。Windows 使用 FlushFileBuffers 和 MOVEFILE_WRITE_THROUGH，但没有可移植的目录 fsync；
当前只对实测的进程终止恢复负责。Linux、ASan/UBSan/TSan、系统重启、掉电与真实文件系统 ENOSPC 的缺测项单列，不能由注入或 Windows 结果替代。

字节格式见 [记录格式 v1](storage-record-v1.md)，故障测试见 [测试说明](../tests/README.md)。
