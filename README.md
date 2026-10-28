# FetchKit

FetchKit 是面向 Android 与 Linux 的 C++ 网络资源下载
SDK，以可靠、可扩展和控制接入体积为目标。首版围绕文件下载、断点续传、多任务调度和资源缓存建立基础能力，后续按需扩展下载能力。

当前状态：主机端已实现多任务调度、独立订阅、持久化重试、检查点/恢复、可选可信 SHA-256、原子发布、
配额、LRU、pin/过期和只读租约，提供 alpha C ABI、C++ 包装和 CLI。 Android 已接入异步 Kotlin/JNI 下载
API、独立 OkHttp 后端、Compose 与 JobScheduler 示例。 M5 已加入 CMake 安装导出、本地 Maven AAR/sources
发布、外部 C/C++/Release R8 宿主、匹配符号归档与发布门槛检查。 当前交付是 `0.1.0` alpha
本地预览；Linux、物理设备矩阵及完整第三方许可审核等门槛尚未满足，没有上传外部仓库。
接入与打包步骤见 [M5 交付指南](docs/m5-release.md)
，验证证据见 [M5 报告](docs/reports/m5-baseline.md)。

项目采用 [Apache License 2.0](LICENSE)
。第三方组件保留各自许可证，见 [依赖说明](third_party/README.md)。

M0 已加入可控 HTTP 服务、主机/Android/JNI 探针、接口契约与体积对照工具。
实测结论、后端调整和缺测项见 [M0 基线报告](docs/reports/m0-baseline.md)；实验接口不等于正式下载能力。

当前用法见 [M3 主机 API](docs/m3-host-api.md)，验证范围见 [M3 报告](docs/reports/m3-baseline.md)。
Android 用法、生命周期和依赖见 [M4 API](docs/m4-android-api.md)
，环境与剩余门槛见 [M4 报告](docs/reports/m4-baseline.md)。 已发布的 Completed 通过 lease
读取，TransportComplete 与 DigestVerified 是独立的完整性结果。
[M1 API](docs/m1-host-api.md)、[M2 存储](docs/m2-storage.md) 和对应报告保留阶段历史；Linux/设备缺测项不由
Windows 结果代替。

首版设计见 [docs/v1-design.md](docs/v1-design.md)。文档中的 API、默认参数和交付顺序均为实现基线，不表示已有能力或实测结果。

分阶段工作包、工程落点、依赖关系和验收门槛见 [首版落地计划](docs/v1-implementation-plan.md)。

## 项目命名

| 项目                         | 统一名称                       |
|------------------------------|--------------------------------|
| 展示名称                     | FetchKit                       |
| 根目录与 Git 仓库名          | `fetchkit`                     |
| CMake 工程名与 C++ 命名空间  | `fetchkit`                     |
| 公共头文件目录               | `include/fetchkit/`            |
| 命令行目标                   | `fetchkit_cli`                 |
| 项目构建选项前缀             | `FETCHKIT_`                    |
| Android SDK 包名与 namespace | `com.luofelix.fetchkit`        |
| Android 演示 App 中文名      | 拾流                           |
| 演示 App namespace           | `com.luofelix.fetchkit.sample` |
| 演示 App applicationId       | `com.luofelix.fetchkit`        |

演示界面使用独立子包，避免与 SDK 的资源和生成类冲突。SDK 包名固定为 `com.luofelix.fetchkit`。

演示 App「拾流」寓意将网络数据流收集到本地，品牌图标以流线、下载箭头和承接托盘表达下载与可靠存储。
图标源文件、配色和 Android 适配说明见 [品牌资源](platform/android/sample/branding/README.md)。

## 工程结构

```text
fetchkit/
├── CMakeLists.txt             # 独立 C++ 核心构建入口
├── include/fetchkit/          # 公共 C ABI 与 C++ 包装
├── src/core/                 # 核心实现
├── src/storage/              # 检查点、记录、扫描、校验、发布和租约
├── src/transport/            # 传输契约与可选 libcurl 后端
├── examples/cli/             # 主机命令行示例
├── platform/android/
│   ├── sdk/                  # :sdk，Kotlin API、JNI、AAR
│   └── sample/               # :sample，Compose 演示 App
├── gradle/                   # 现有 Wrapper、版本目录与 JVM 配置
├── settings.gradle.kts       # 根目录 Gradle 工程入口
└── docs/
```

Android Studio 直接打开仓库根目录。Gradle 模块通过 `projectDir` 映射到 `platform/android/`，无需再建立一套嵌套
Gradle 工程。

调用链：演示 App → `com.luofelix.fetchkit.FetchKit.version()` → JNI → C++ 核心。 主机与 Android 复用同一
C++ 下载核心；Android 模块提供平台网络与 JNI 适配。

## 构建与运行

沿用当前工程的 AGP、Gradle 与 Kotlin 版本，NDK 固定为 `28.2.13676358`、CMake 固定为 `3.22.1`，版本统一记录在
`gradle/libs.versions.toml`。当前 Android 构建包含 `arm64-v8a` 和 `x86_64`，最低 API 24。

在仓库根目录的 PowerShell 中执行：

```powershell
.\gradlew.bat :sdk:assembleDebug :sdk:testDebugUnitTest :sample:assembleDebug :sample:testDebugUnitTest
```

Android Studio 中选择 `sample` 运行。示例提供下载、存储和能力指南三个页面，内置清华镜像的 Ubuntu
24.04.5 Netboot / WSL 和阿里云镜像的 Netboot 预设，可直接多选下载，自定义 URL 为可选。支持逐任务和批量暂停恢复、独立订阅、
检查点/重试信息、可信 SHA-256、pin/过期、LRU
回收与租约读取，操作路径见 [示例指南](docs/android-sample.md)。AAR 位于
`platform/android/sdk/build/outputs/aar/`，APK 位于 `platform/android/sample/build/outputs/apk/`。

有可用设备或模拟器时，单独验证安装标识与 JNI 加载：

```powershell
.\gradlew.bat :sample:connectedDebugAndroidTest '-Pandroid.testInstrumentationRunnerArguments.class=com.luofelix.fetchkit.sample.SdkIntegrationTest'
```

独立 C++ 构建不依赖 Android SDK。本机 Windows 使用 Ninja 和 g++：

```powershell
cmake -S . -B build/native-debug -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native-debug --parallel
.\build\native-debug\fetchkit_cli.exe --version
```

Linux 可省略编译器参数，运行 `./build/native-debug/fetchkit_cli --version`。CLI 默认不链接
curl，可查询版本；增加 `-DFETCHKIT_WITH_CURL=ON` 后 CLI 可下载与查询同进程任务。 固定源码参考配置使用
`-DFETCHKIT_USE_PINNED_CURL=ON`，具体入口见 [依赖说明](third_party/README.md)。 可选 CTest 与 Android
探针运行方式见 [测试说明](tests/README.md)， 体积对照见 [测量工具说明](tools/size/README.md)。普通构建不含
M0 探针；正式 Android 后端已将 OkHttp 5.5.1 及其传递依赖纳入运行图。 完整设备协议与进程恢复分别使用
`tools/ci/run_m4_android.py` 和 `tools/ci/run_m4_recovery.py`，需要受控本地服务器。

## 首版目标

- 下载普通文件：视频文件、音频、图片、地图数据包、模型、游戏资源和压缩包等。
- 支持暂停、恢复、取消、受控重试、进度通知和进程退出后的恢复。
- 正确处理服务端不支持续传、资源发生变化、磁盘不足等情况。
- 在文件完成必要校验并发布后，才向调用方提供可用资源。
- 提供有配额、淘汰和保留能力的资源存储。
- C++ 实现任务、续传、存储和缓存策略；网络传输通过后端适配。
- Android 使用经 M0 选择的隔离 OkHttp 后端并单列完整依赖成本；Linux 使用 libcurl 后端。

首版聚焦普通文件，HLS/DASH 清单与分片编排、DRM、多连接分段加速、差分更新及解压安装列入后续范围。

## 交付顺序

1. 验证网络后端契约、取消延迟与 Android 包体积基线。
2. 完成 Linux 单文件下载和测试服务器，验证续传协议。
3. 完成持久化、校验与崩溃恢复。
4. 完成调度、资源存储、配额与淘汰。
5. 完成 JNI、Kotlin API、后台调度示例与 Android 集成验证。
6. 发布 Linux CLI、Android AAR、接入文档和实测体积报告。

以故障测试的通过情况作为交付依据，具体场景见设计文档。
