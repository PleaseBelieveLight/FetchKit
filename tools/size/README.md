# Android 接入体积基线

在仓库根目录运行：

```powershell
python tools/size/measure.py --abi arm64-v8a
```

工具顺序构建同一个 Java Activity 宿主的四种配置：bare（不接入）、scaffold（现有版本接口）、probe（HttpURLConnection/JNI 实验）、okhttp（隔离客户端/JNI 实验）。`size-host` 仅在传入 `fetchkit.sizeMode` 时加入 Gradle 工程。宿主关闭内建 Kotlin 编译，避免空宿主预先带入 SDK 需要的 Kotlin 标准库；接入后的所有传递依赖均计入增量。

统一使用 Release、单 ABI、unsigned APK、R8 关闭的口径。每种配置复制 APK/AAR、依赖树、构建日志，计算 SHA-256 和 native ZIP 项大小；静态核心归档及实际 native CMake 配置另列，不能替代 strip 后 JNI 指标。最后另外构建普通 SDK 的双 ABI AAR。结果在 `build/m0-size/report.json`，二进制不提交 Git。M0 报告可收录 JSON 摘要和原始样本。

Gradle daemon 要求 JDK 25；可将当前终端 `JAVA_HOME` 指向已有安装，避免重复下载。工具记录实际 Java 版本和源码哈希，不能只按构建目录名推测 native 优化等级。

这些是未压缩优化的基线，不是最终发布成本承诺。设备安装大小、R8 优化结果、全部 ABI AAR、调度适配和 Android curl 配置需要分别实测；本工具不能推导这些数字。

普通构建不设置 `fetchkit.probes`；实验必须明确设置 `-Pfetchkit.probes=true`。OkHttp 只在同时选择 `-Pfetchkit.probeBackend=okhttp` 时成为 SDK 实验构建依赖，不进入普通 SDK。
