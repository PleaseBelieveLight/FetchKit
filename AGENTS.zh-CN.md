# 仓库贡献指南

## 文档同步

英文版本：[AGENTS.md](AGENTS.md)。修改任一版本时，必须同时更新另一版本，确保要求、示例和命令保持一致。

## 项目结构与模块组织

FetchKit 已建立 C++/Android 工程骨架，下载功能尚未实现。修改范围或架构前，先阅读 `README.md` 和
`docs/v1-design.md`。

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
```

以上命令构建 AAR/APK、运行单元与设备测试，以及编译 native CLI。设备测试需要连接设备。
执行 `build/native-debug/fetchkit_cli.exe --version` 验证 native 构建。

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
当前尚无 C++ 测试框架、CTest 注册或覆盖率门槛。后续协议测试使用 `resume_test.cpp` 等文件名和
可控制行为的本地 HTTP 服务器。

覆盖服务端忽略 Range、无效的 206/416
响应、资源变化、检查点写入中断、摘要校验失败、磁盘耗尽和取消竞态。遵循 `docs/v1-design.md`
的验收清单，分别记录进程终止测试与系统重启测试。

## 提交与 Pull Request 要求

使用 `type: description` 格式，例如 `refactor: separate SDK and sample`。提交主题概括最终变更。

PR 应说明行为、范围、验证结果和关联议题。修复 BUG 时概括原因与修复方法。
依赖和包体积变化需提供实测结果。修改项目代码后，给出建议的 Git 提交信息。

## 架构与安全

下载策略和存储逻辑放在 C++ 核心中，平台网络与调度通过独立适配层接入。文件通过所需校验后才能发布。凭证和签名
URL 应脱敏。包体积测量必须计入所有新增的必要依赖。
