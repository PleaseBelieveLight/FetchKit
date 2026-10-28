# Android 接入体积基线

[文档中心](../../docs/README.md) · [验证报告索引](../../docs/reports/README.md)

M4 正式下载接入使用独立文件 AAR 宿主，分别比较同工具链、arm64/API24、R8 关闭和开启时的 bare/production：

```powershell
python tools/size/measure_m4.py --ndk D:/Android/Sdk/ndk/28.2.13676358
```

测量包括完整 JNI 核心、Kotlin API、OkHttp/Okio/Kotlin/AndroidX 运行依赖；最小 Java 宿主不预装 Kotlin，
不包含 Compose/WorkManager。静态 STL 已计入 JNI，记录 strip 后导出/NEEDED、双 ABI AAR 与单 ABI APK。
SDK Native Release 的实际 CMake 配置需单列，不能与 M3 独立 `-Os` 假传输目标混成同一指标。
`fetchkit.sizeMode=production` 支持 `fetchkit.aar` 文件接入和 `fetchkit.r8=true`；`fetchkit.smokeSign=true`
只为设备烟测启用本机 debug 签名，不改变正式 AAR，也不进行外部发布。

M5 复测证据见 [报告](../../docs/reports/m5-baseline.md)，外部 Maven/R8 运行使用
`tools/ci/verify_android_package.py`。`tools/release/package_preview.py` 将测量依赖树、匹配符号与发布清单一同归档。

M3 完整主机核心/ABI 的独立 arm64 测量在同一入口增加 `--include-engine --output build/m3-size-arm64`。
engine 目标保留 Scheduler、存储、Verifier 和公开 C ABI，使用确定性假传输占位；不包含 Android 网络后端或 JNI 下载桥。
与 baseline/digest 使用相同导出列表、静态 STL、优化和 strip。该口径与普通 JNI 版本入口、最终 AAR/APK 接入成本分别报告。

M2 摘要与文件读取的独立测量：

```powershell
python tools/size/measure_m2.py --ndk D:/Android/Sdk/ndk/28.2.13676358 --cmake D:/Android/Sdk/cmake/3.22.1/bin/cmake.exe
```

使用同一 arm64-v8a/API24、Release `-Os`、静态 STL、section GC、无 LTO、相同 strip 的两个共享库比较：
baseline 仅版本入口，digest 实际引用完整文件 SHA-256 Verifier。结果包含命令、源码/产物哈希、NEEDED、
静态核心和现有普通 AAR；保存在 `build/m2-size-arm64/report.json`。这项测量包含文件读取/异常/运行库实际链接成本，
不是只统计第三方头文件字节数，也不等同于完整 Android 下载接入增量。当前普通 JNI 仍仅引用版本接口，不能用它的零增量宣称摘要零成本。

以下保留 M0 完整宿主对照工具。

在仓库根目录运行：

```powershell
python tools/size/measure.py --abi arm64-v8a
```

工具顺序构建同一个 Java Activity 宿主的四种配置：bare（不接入）、scaffold（现有版本接口）、probe（HttpURLConnection/JNI 实验）、okhttp（隔离客户端/JNI 实验）。`size-host` 仅在传入 `fetchkit.sizeMode` 时加入 Gradle 工程。宿主关闭内建 Kotlin 编译，避免空宿主预先带入 SDK 需要的 Kotlin 标准库；接入后的所有传递依赖均计入增量。

统一使用 Release、单 ABI、unsigned APK、R8 关闭的口径。每种配置复制 APK/AAR、依赖树、构建日志，计算 SHA-256 和 native ZIP 项大小；静态核心归档及实际 native CMake 配置另列，不能替代 strip 后 JNI 指标。最后另外构建普通 SDK 的双 ABI AAR。结果在 `build/m0-size/report.json`，二进制不提交 Git。M0 报告可收录 JSON 摘要和原始样本。

Gradle daemon 要求 JDK 25；可将当前终端 `JAVA_HOME` 指向已有安装，避免重复下载。工具记录实际 Java 版本和源码哈希，不能只按构建目录名推测 native 优化等级。

这些是未压缩优化的基线，不是最终发布成本承诺。设备安装大小、R8 优化结果、全部 ABI AAR、调度适配和 Android curl 配置需要分别实测；本工具不能推导这些数字。

M0 历史配置中，OkHttp 仅随 `-Pfetchkit.probes=true -Pfetchkit.probeBackend=okhttp` 进入实验构建。
M4 起 OkHttp 5.5.1 已进入普通 SDK 运行图；普通构建仍不包含实验探针。
