# 构建与运行

[文档中心](../README.md)

以下命令均在仓库根目录执行。接入已构建的 native/AAR 产物见 [接入与打包](../release/packaging.md)。

## Android

Android Studio 直接打开仓库根目录，选择 `sample` 运行拾流演示 App。 Gradle 将 `:sdk`、`:sample` 分别映射到
`platform/android/sdk/`、`platform/android/sample/`，不另建嵌套工程。

版本统一在 `gradle/libs.versions.toml`：沿用项目的 AGP、Gradle、Kotlin 配置，NDK 为 `28.2.13676358`，
CMake 为 `3.22.1`；最低 API 24，默认 ABI 为 `arm64-v8a` 和 `x86_64`。

在 PowerShell 中构建并运行单元测试：

```powershell
.\gradlew.bat :sdk:assembleDebug :sdk:testDebugUnitTest :sample:assembleDebug :sample:testDebugUnitTest
```

AAR 位于 `platform/android/sdk/build/outputs/aar/`，APK 位于
`platform/android/sample/build/outputs/apk/`。设备或模拟器连接后，可验证应用标识与 JNI 加载：

```powershell
.\gradlew.bat :sample:connectedDebugAndroidTest '-Pandroid.testInstrumentationRunnerArguments.class=com.luofelix.fetchkit.sample.SdkIntegrationTest'
```

示例提供下载、存储和能力指南三个页面，具体操作见 [Android 示例](android-sample.md)。 正式网络后端使用
OkHttp 5.5.1 及其传递依赖；SDK 不依赖 Compose、WorkManager 或通知组件。

## 主机 C++ / CLI

独立 CMake 构建不依赖 Android SDK。Windows 使用 Ninja 和 g++：

```powershell
cmake -S . -B build/native-debug -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native-debug --parallel
.\build\native-debug\fetchkit_cli.exe --version
```

Linux 可省略编译器参数，运行 `./build/native-debug/fetchkit_cli --version`。 默认构建不链接
curl，仅用于版本等基础验证；下载能力需增加 `-DFETCHKIT_WITH_CURL=ON`。 固定 curl 源码构建增加
`-DFETCHKIT_USE_PINNED_CURL=ON`，环境与依赖见
[第三方依赖](../../third_party/README.md)，实际调用见 [主机 API](../reference/host-api.md)。

## 工程结构与命名

```text
fetchkit/
├── include/fetchkit/       # 公共 C ABI 与 C++ 包装
├── src/core/              # 核心调度与下载策略
├── src/storage/           # 记录、检查点、校验、发布与租约
├── src/transport/         # 传输契约与 libcurl 后端
├── platform/android/
│   ├── sdk/               # Kotlin/JNI、OkHttp 适配、AAR
│   └── sample/            # 拾流 Compose / JobScheduler 示例
├── examples/              # CLI 与独立接入示例
├── tests/                 # 协议、恢复、存储与集成验证
├── tools/                 # CI、发布与体积测量
└── docs/                  # 主题文档与导航
```

| 项目                           | 名称                                                     |
|--------------------------------|----------------------------------------------------------|
| 项目 / CMake / C++ namespace   | `fetchkit`                                               |
| CLI / 构建选项前缀             | `fetchkit_cli` / `FETCHKIT_`                             |
| SDK 包名与 namespace           | `com.luofelix.fetchkit`                                  |
| 示例 namespace / applicationId | `com.luofelix.fetchkit.sample` / `com.luofelix.fetchkit` |
| 示例展示名                     | 拾流                                                     |

示例使用独立包，避免 SDK 资源及生成类冲突。图标与配色见
[品牌资源](../../platform/android/sample/branding/README.md)。

## 验证入口

- [持续集成](continuous-integration.md)：GitHub Actions 日常检查、扩展验证与手动 Alpha 分发。
- [测试说明](../../tests/README.md)：CTest、Android 探针、协议与恢复脚本；普通 SDK 构建不含探针或故障注入。
- [体积测量](../../tools/size/README.md)：按完整宿主依赖测量接入增量。
- [验证报告](../reports/README.md)：已测环境、原始结果及缺测项。
- [发布检查表](../release/checklist.md)：Linux、设备矩阵、大文件、重启与许可等交付门槛。

Windows 结果不代替 Linux 验收；进程终止、系统重启和掉电分别提供证据。
