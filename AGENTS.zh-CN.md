# 仓库贡献指南

## 文档同步

英文版本：[AGENTS.md](AGENTS.md)。修改任一版本时，必须同时更新另一版本，确保要求、示例和命令保持一致。

## 项目结构与模块组织

FetchKit 已实现 M2 主机下载器，包括持久化检查点、SHA-256 校验、发布和读取租约，Android 仍为工程骨架。
调度、配额和缓存尚未实现。修改范围或架构前，先阅读 `README.md`、`docs/v1-design.md`、
`docs/m2-storage.md` 和 `docs/storage-record-v1.md`。

根目录 CMake 工程构建 `include/fetchkit/`、`src/core/` 和 `examples/cli/`。
根目录 Gradle 配置将 `:sdk` 映射到 `platform/android/sdk/`，将 `:sample` 映射到
`platform/android/sample/`。JNI 和 Kotlin 接口放在 SDK 中，Compose 界面放在示例中。
实现存储、缓存、传输和协议测试时，再创建相应目录。

## 构建、测试与开发命令

在 Windows 的仓库根目录执行：

```powershell
.\gradlew.bat :sdk:assembleDebug :sample:assembleDebug :sample:testDebugUnitTest
.\gradlew.bat :sample:connectedDebugAndroidTest
cmake -S . -B build/native-debug -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native-debug --parallel
cmake -S . -B build/m0-host -G Ninja -DCMAKE_CXX_COMPILER=g++ -DFETCHKIT_BUILD_TESTS=ON -DFETCHKIT_BUILD_PROBES=ON -DFETCHKIT_WITH_CURL=ON
cmake --build build/m0-host --parallel
ctest --test-dir build/m0-host --output-on-failure -LE large_file
```

以上命令构建 AAR/APK、运行单元与设备测试，以及编译 native CLI。设备测试需要连接设备。
执行 `build/native-debug/fetchkit_cli.exe --version` 验证 native 构建。
M0 探针需要 Python 3.10+ 和 libcurl 7.85+ 开发文件；必要时设置 `CMAKE_PREFIX_PATH`。
可选 Android 探针见 `tests/README.md`，体积测量见 `tools/size/README.md`。
M1 参考构建可开启 `FETCHKIT_USE_PINNED_CURL`，Linux 验收入口为 `tools/ci/linux_m1.sh`。
真实 >4 GiB 测试需开启 `FETCHKIT_BUILD_LARGE_FILE_TESTS=ON` 并选择 `large_file` 标签。
不得用 Windows 证据代替 Linux 验收。
M2 恢复测试还需在主机测试构建中开启 `FETCHKIT_ENABLE_FAULT_INJECTION=ON`，普通 SDK 必须关闭。
`tools/ci/linux_m2.sh` 包含恢复验证；Linux 上分别使用 `FETCHKIT_SANITIZERS=address,undefined` 和
`FETCHKIT_SANITIZERS=thread` 执行诊断。主机需要 zlib 开发文件，Android 使用系统 libz。
PicoSHA2 的固定哈希与 MIT 许可证位于 `third_party/`。

## 编码风格与命名规范

根目录、Git 仓库、CMake 工程名和 C++ 命名空间统一使用 `fetchkit`；命令行目标使用 `fetchkit_cli`
，项目构建选项使用 `FETCHKIT_` 前缀。

SDK 包名固定为 `com.luofelix.fetchkit`。示例 namespace 为 `com.luofelix.fetchkit.sample`，
applicationId 保留为 `com.luofelix.fetchkit`。

新增代码使用 C++17 和四空格缩进；类型使用 `PascalCase`，函数使用 `lowerCamelCase`，文件名和字段使用
`snake_case`。使用 RAII 管理资源所有权，文件偏移使用 64 位类型。禁止异常跨越 C ABI 或 JNI 边界。

当前未配置格式化或静态检查工具，引入时同时提交配置。架构变化时同步更新设计文档。

## 测试要求

Android 使用 JUnit4，示例设备测试验证安装标识和 SDK JNI 加载。
CTest 已注册服务器、纯 C ABI、假传输会话、版本、真实协议、存储黄金样本/SHA 向量、确定性子进程恢复及可选 curl 契约/大文件测试。
当前未配置第三方 C++ 测试框架或覆盖率门槛。协议测试使用 `resume_test.cpp` 等文件名和可控制行为的本地 HTTP 服务器。

M0 实验源码和依赖不得进入普通 SDK 构建。Android 探针仅通过 `-Pfetchkit.probes=true` 开启，
使用 `fetchkit.probeBackend` 选择实验后端。明确报告契约门槛失败和缺失环境，不能放宽断言以宣称通过。

覆盖服务端忽略 Range、无效的 206/416
响应、资源变化、检查点写入中断、摘要校验失败、磁盘耗尽和取消竞态。遵循 `docs/v1-design.md`
的验收清单，分别记录进程终止测试与系统重启测试。
仅 Completed 对象可 acquire。禁止从记录恢复回调/凭证；数据与记录同步完成前不得推进 committed 偏移。
保留未知版本数据。

## 提交与 Pull Request 要求

使用 `type: description` 格式，例如 `refactor: separate SDK and sample`。提交主题概括最终变更。

PR 应说明行为、范围、验证结果和关联议题。修复 BUG 时概括原因与修复方法。
依赖和包体积变化需提供实测结果。修改项目代码后，给出建议的 Git 提交信息。

## 架构与安全

下载策略和存储逻辑放在 C++ 核心中，平台网络与调度通过独立适配层接入。文件通过所需校验后才能发布。凭证和签名
URL 应脱敏。包体积测量必须计入所有新增的必要依赖。
