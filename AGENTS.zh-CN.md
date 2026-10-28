# 仓库贡献指南

## 文档同步

英文版本：[AGENTS.md](AGENTS.md)。修改任一版本时同步更新另一版本，保持要求、示例与命令一致。
这两份文件聚焦长期有效的协作规则；实现历史、环境兼容细节和操作步骤放在 `docs/` 或相关工具 README，
此处通过链接引用。

## 工作方式与必读资料

- 修改前检查工作区，保留与本任务无关的改动。控制变更范围，避免顺带重构、全量格式化， 或为满足风格规则重命名已有
  API。
- 先看 [文档中心](docs/README.md)，再按变更范围阅读相关资料：

| 变更范围                       | 必读资料                                                                                                                                                       |
|--------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 产品范围或架构                 | [设计基线](docs/design/v1-design.md) 和受影响的 API 契约；范围/阶段变化时读 [落地计划](docs/design/v1-implementation-plan.md)                                  |
| C/C++ API、调度或存储          | [主机 API](docs/reference/host-api.md)；持久化变化时读 [记录 v2](docs/reference/storage-record-v2.md)，迁移时读 [记录 v1](docs/reference/storage-record-v1.md) |
| Kotlin/JNI、Android 网络或示例 | [Android API](docs/reference/android-api.md)，按需读 [示例指南](docs/guides/android-sample.md)                                                                 |
| 传输或 HTTP 行为               | [传输契约](docs/reference/transport-contract.md) 和受影响的主机/Android API                                                                                    |
| 打包、依赖或发布声明           | [接入与打包](docs/release/packaging.md)、[发布检查表](docs/release/checklist.md)、[依赖说明](third_party/README.md)                                            |

- 区分当前契约、设计目标和历史证据。声明、实现、测试与文档不一致时，明确差异并在任务范围内修正；
  不得静默重定义行为或放宽断言以通过检查。

## 项目结构与职责

- `include/fetchkit/` 放公共 C ABI 和 C++ 包装。根目录 CMake 构建 `src/core/`、`src/storage/`、 可选的
  `src/transport/` 后端与 `examples/cli/`。
- 下载策略、调度、重试、持久化和缓存策略归 C++；平台网络与系统调度通过独立适配层接入， 不在 Kotlin
  或示例中重复实现核心策略。
- 根目录 Gradle 将 `:sdk` 映射到 `platform/android/sdk/`，将 `:sample` 映射到
  `platform/android/sample/`。Kotlin/JNI 与隔离 OkHttp 适配放在 SDK，Compose UI 和 JobScheduler 示例
  放在 sample。SDK 不得强制依赖示例 UI、WorkManager 或通知组件。
- 只为实际实现新增模块，不预建未来使用的空目录。

## 文档结构

- 根目录 `README.md` 保持轻量，基础 SDK 接入与使用说明应自成一体，包含项目简介、最小接入步骤和代码示例、
  架构说明与 SDK 核心接口。除非确实需要，不应引导用户去看其它文档；基础接入与最小示例应仅凭 README
  即可完成。
- `docs/README.md` 是统一入口：`guides/` 放开发与使用指南，`reference/` 放 API 与契约，
  `design/` 放架构与计划，`release/` 放打包与门槛，`reports/` 放验证证据，`archive/` 放被替代的指南。 不在
  `docs/` 根目录新增零散主题文档。
- 当前指南/API 使用稳定的主题文件名，如 `reference/android-api.md`。历史内容明确标注并链接当前 API；
  阶段名称保留给历史指南与报告。
- 每篇文档聚焦单一主题，使用简短说明和最小必要示例。同一流程或契约只保留一个权威位置，其他文档引用链接。
  行为或架构变化时，同步更新受影响的 API、设计与使用文档。
- 新增、移动或重命名文档时，更新索引及受影响的链接、锚点和文字路径，包括相关示例提示和 AGENTS 引用。
  主题页提供返回索引的路径；检查本次变更涉及的本地链接、锚点和示例。
- 整理结构时保持原始证据的路径与内容不变，新验证使用独立输出路径，避免覆盖历史。测试、体积工具、依赖或资源的说明
  可保留在相邻 README 中，从文档索引链接，不重复复制。

## 编码风格与命名

- 仓库、CMake 工程和 C++ 命名空间使用 `fetchkit`，CLI 使用 `fetchkit_cli`，构建选项使用 `FETCHKIT_`。
- SDK 包名/namespace 固定为 `com.luofelix.fetchkit`；示例 namespace 为
  `com.luofelix.fetchkit.sample`， applicationId 保留为 `com.luofelix.fetchkit`。
- C++ 使用 C++17、四空格缩进、RAII 和 64 位文件偏移。新增类型使用 `PascalCase`，函数使用
  `lowerCamelCase`，文件名/字段使用 `snake_case`；保留已有局部约定，如私有成员后缀。 公共 C ABI 名称保留
  `fetchkit_` 前缀与 `snake_case`。
- Kotlin 类型使用 `PascalCase`，函数与属性使用 `lowerCamelCase`，文件名沿用相邻代码约定。
  保留已有公共名称和机器可读格式。
- 禁止异常跨越 C ABI 或 JNI 边界。当前未配置格式化/静态检查工具，引入时同步提交配置，避免无关格式化。

## 核心正确性约束

- 工作线程、缓冲区、队列和保留事件必须有界。主机通过 `fetchkit_task_dispatch` 主动分发事件； 不得在 SDK
  锁下调用宿主代码或引入无界回调队列。
- 暂停/恢复影响共享任务；取消只撤销一个订阅，最后一个显式取消才清理共享未完成任务。 最后一个释放停止
  I/O 并尽力保留检查点。保持 generation 隔离及 provider/回调取消屏障。
- 数据与记录同步完成前不得推进 committed 偏移；保持发布/恢复顺序，仅有效 Completed 对象可 acquire。
  传输完成与可信摘要验证分别呈现。
- 使用记录 v2 保存重试/pin/过期语义，迁移 v1 不得丢失关键字段；保留未知版本数据， 不得从记录恢复回调、provider
  或凭证。
- 写入前预留配额，计入元数据与未知文件，删除/淘汰时保护活动任务、pin 和读取租约。 engine 关闭后仍须保持已有
  lease 的约定生命周期。

## 注释与日志

- 代码注释和面向人的日志以中文为主；保留关键技术术语、API 名称、标识符、协议字段、错误码和机器可读格式。
- 在公共 C/C++/Kotlin 声明处说明契约，按需使用 KDoc：参数范围/单位、返回/错误、线程/回调约束、
  所有权与释放要求。说明共享暂停/恢复、取消与释放区别、provider 协作取消和 lease 生命周期， 包括竞态及实际可能结果。
- 为不直观的约束添加注释：锁顺序、generation 隔离、取消屏障、数据/记录同步、发布/恢复顺序、
  配额保护和协议兼容处理。解释所防范的问题；行为变化时同步更新，清理过时说明和注释掉的代码。
  不复述明显语句，不要求每个函数都有注释。
- 在有效故障边界记录诊断：engine/存储打开或关闭、恢复异常/隔离、重试耗尽、重要回退、校验/发布、
  provider/listener/executor 失败。在上下文充分的一层记录一次，保留错误/快照/事件反馈；
  不得在既无可观察错误也无安全诊断的情况下静默丢弃意外异常。
- SDK 诊断默认关闭，由宿主控制。失败使用 Error，可恢复异常使用 Warn，细节使用 Debug；预期的暂停/取消不是错误。
  避免逐缓冲区、逐次读写、逐轮询或逐进度输出；重复消息限频，保留历史必须有界。
- 显式选择安全上下文：操作/阶段、核心生成的 taskId/persistentId、generation、状态、HTTP/平台码和重试次数。
  禁止记录鉴权 token、Cookie、鉴权头、完整签名 URL、响应体、请求对象转储或未脱敏的异常消息/堆栈， 包括通过
  `toString()` 间接输出。
- 格式化与输出保持轻量并在 SDK 锁外执行，不在高频路径添加阻塞 I/O 或无界队列； CLI/测试的机器可读输出与诊断日志分开。

## 验证要求

命令以 [构建与运行](docs/guides/development.md) 和 [测试说明](tests/README.md) 为准，在仓库根目录执行。
按受影响行为选择检查，不要求每次编辑重跑全部历史阶段入口：

| 变更范围                   | 验证要求                                                                                                       |
|----------------------------|----------------------------------------------------------------------------------------------------------------|
| 仅文档                     | 本地链接/锚点、引用路径、变更示例与 `git diff --check`；除验证示例所需外，无需完整 native/Android 构建         |
| C++ 核心或 C ABI           | 相关 CTest 单元/API 测试；HTTP、存储或取消行为变化时运行相关协议/恢复测试                                      |
| Kotlin/JNI 或 Android 传输 | SDK 构建/单元测试及相关设备集成/协议测试；主机测试不能代替 JNI 或 Android 网络验证                             |
| 示例 UI/生命周期           | sample 构建/单元测试；交互/生命周期变化时运行相关设备测试                                                      |
| 打包或依赖                 | 按影响范围验证 native 安装/外部 C/C++ 宿主和/或双 ABI AAR/外部 Release R8 宿主、包内容/许可及完整依赖/体积实测 |

- 逻辑变更和 BUG 修复应补充或更新回归覆盖，复用可控本地 HTTP 服务与现有 JUnit4/CTest 设施。
  覆盖 [设计验收清单](docs/design/v1-design.md) 中受影响的失败/竞态场景，不只验证成功路径。
  低风险文案/样式改动使用相关现有检查即可，避免编写仅重复实现的测试。
- 确认预期测试实际执行：筛选 CTest 时使用 `ctest --no-tests=error`，Android 核对测试类与用例数量。
  明确报告失败、缺失环境和未执行项；不得降低门槛或将编译成功当作运行验证。
- 主机故障注入/sanitizer 构建与正式产物隔离。恢复测试需 `FETCHKIT_ENABLE_FAULT_INJECTION=ON`； Linux
  上分别运行 ASan/UBSan 与 TSan。实际 >4 GiB 测试需 `FETCHKIT_BUILD_LARGE_FILE_TESTS=ON` 和
  `large_file` 标签，模拟偏移不等于大文件实测。Linux 验收入口为 `tools/ci/linux_m5.sh`。
- 记录平台/API/ABI，区分 Windows 与 Linux、模拟器与真机、SIGKILL 与 force-stop、进程终止与重启/掉电。 不得用
  Windows 证据替代 Linux 验收，也不得用模拟器结果替代真机门槛。 复用已有设备，使用测试指南链接的受控
  AVD 启动器和 Android 驱动；仅清理自有进程及
  `adb reverse --no-rebind` 映射，仅测量自有子进程。自动重启仅限明确选中的模拟器 guest， 物理重启与掉电须另有证据。

## 依赖、打包与发布

- 版本以 `VERSION`、`gradle/libs.versions.toml` 和依赖锁文件为准，不在此重复维护版本快照。 主机需要
  zlib，Android 使用系统 libz；保留包括 PicoSHA2 在内的第三方固定哈希和许可证。
- 探针专用源码/依赖不得进入普通 SDK 构建。Android 探针须显式开启 `-Pfetchkit.probes=true`， 通过
  `fetchkit.probeBackend` 选择实验；正式 OkHttp 是独立的运行依赖。测试 CA/网络例外仅放 sample Debug，
  SDK/Release 产物不得包含探针、故障钩子或测试信任锚。
- `FETCHKIT_INSTALL=ON` 仅用于显式选择的正式产物配置，也包括本地预览；禁止安装故障注入或 sanitizer 构建。
  本地 Maven 发布使用 `:sdk:publishReleasePublicationToPreviewRepository`，要求两个支持的 ABI 且关闭
  probes。
- 按 [打包指南](docs/release/packaging.md) 验证外部宿主，归档 ELF Build ID 匹配的符号、依赖树与 SHA
  清单。 接入体积计入完整宿主的运行和传递依赖，与核心单体体积区分。真实 ENOSPC 证据需要 Linux
  用户/挂载命名空间， 不能用故障注入替代。
- FetchKit 按根目录 `LICENSE`（Apache-2.0）授权，第三方许可分别保留。native 安装、AAR Java 资源和 sources
  JAR 包含相同的项目许可原文，Maven POM 同步声明。缺少必要证据时
  `tools/release/check_release.py` 必须阻断发布；预览验证通过不等于发布验收通过。 没有所有者发布指令时不得上传产物。

## 提交与 Pull Request 要求

沿用历史的 `type: description` 格式，例如 `docs: 规范文档维护与验证规则`，主题聚焦最终变更。
修改项目代码后给出建议的 Git 提交信息。

PR 与任务结果说明变更行为/范围、已执行检查和剩余限制；BUG 修复概括原因与修复方法。
有相关议题时附上链接；依赖/体积变更提供实测数据，不得声称未执行的检查已经通过。
