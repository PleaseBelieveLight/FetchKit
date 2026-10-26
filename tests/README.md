# FetchKit 主机与平台验证

M1 主机验证还包括纯 C ABI、假传输状态机、真实下载协议/HTTPS/CLI 和独立大文件集合。
接口与限制见 [M1 API](../docs/m1-host-api.md)，固定依赖与 Linux
入口见 [依赖说明](../third_party/README.md)。
`ctest --test-dir build/m1-host --output-on-failure -LE large_file` 运行常规集合； 要注册实际 >4 GiB
测试，配置时显式增加 `-DFETCHKIT_BUILD_LARGE_FILE_TESTS=ON`，再执行
`ctest --test-dir build/m1-host --output-on-failure --no-tests=error -L large_file`。
未启用该选项时不会悄悄执行大文件传输；`--no-tests=error` 可避免将空测试集合误报为通过。

测试服务器仅用 Python 3.10+ 标准库。固定响应定义在 `fixtures/m0_scenarios.json`；Linux 和 Android
驱动共用场景 ID、内容规则与断言，驱动本身独立。

## 主机

Linux 安装 C++17 编译器、Ninja、CMake 3.22.1+、Python 3.10+ 和 libcurl 7.85+ 开发包后，在仓库根目录运行：

```sh
cmake -S . -B build/m0-host -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFETCHKIT_BUILD_TESTS=ON -DFETCHKIT_BUILD_PROBES=ON -DFETCHKIT_WITH_CURL=ON
cmake --build build/m0-host --parallel
ctest --test-dir build/m0-host --output-on-failure
```

Windows 可增加 `-DCMAKE_CXX_COMPILER=g++` 和本机 libcurl 安装的 `-DCMAKE_PREFIX_PATH=...`；动态运行库需在
PATH。主机探针并不意味着 Windows 下载成为正式产品承诺。

`unit` 包括服务器单测与版本烟测，`contract` 包括 13 个场景、五类取消各 100 次。原始样本写入构建目录
`reports/curl-contract.json`；测试失败返回非零。FNV-1a 用于比较测试内容，不能代表可信摘要校验。

服务器可单独运行，自动分配端口并输出就绪信息：

```powershell
python tests/server/http_fixture.py --ready-file build/fixture-ready.json
```

只监听回环地址，不记录完整 URL、请求凭证或异常消息。可通过 Ctrl+C 停止。延迟/停顿连接检测客户端关闭，后台线程在服务退出时释放。

## Android

先启动一个已有设备/模拟器，确保 `adb` 在 PATH，然后运行：

Gradle daemon 要求 JDK 25；若本机已安装但未自动发现，可先将当前终端的 `JAVA_HOME` 指向该
JDK，避免工具链重复下载。

```powershell
python tests/contract/run_android_probe.py --serial emulator-5554 --backend platform --repetitions 100 --output build/m0-android
python tests/contract/run_android_probe.py --serial emulator-5554 --backend okhttp --repetitions 100 --output build/m0-okhttp
```

驱动会构建并安装示例及测试 APK，建立独占的临时 adb reverse 规则，运行 instrumentation，提取带 run_id
的报告并回收规则。它保留测试 APK 和 `files/m0-platform.json`，不会卸载其他应用。输出包含 `gradle.log`、
`instrumentation.log` 和 `report.json`；测试进程独占全局 Cookie/Authenticator 实验，不应与其他
instrumentation 并发。

Android 退出码区分不了所有 instrumentation 断言，因此驱动同时检查 JSON gate；旧 run_id
或缺失结果均视为失败。全局凭证策略不满足契约时应失败，不能删除断言使门槛变绿。默认构建和普通设备烟测不包含这些实验源码/依赖。

报告记录每次取消到 worker 与清理线程都停止的耗时，分别统计 p95/max。头屏障、背压由显式 latch
命中；阻塞读取先收到首块再等待 30 ms 后取消。真实 DNS、TLS、弱网、系统停止、检查点和进程恢复仍需后续测试，不能由
M0 推导。
