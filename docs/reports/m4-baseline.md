# M4 Android 验证范围

[文档中心](../README.md) · [验证报告索引](README.md)

2026-10-27。实现基准提交 `0f49424`；本页收录在 M5 交付验证期间完成的 Android 复测。
SDK 仍为 alpha，具体调用与生命周期规则见 [M4 API](../reference/android-api.md)。

API 31 x86_64 模拟器的正式 Kotlin/JNI/C++/OkHttp 5.5.1 链路通过 74 项协议与生命周期检查，
包括 UTF-8 请求头、隐藏 503 重试防护、JobScheduler 系统停止、备份部分恢复、关闭竞争与租约保护。
驱动只运行指定测试类；172 次受控请求，断言 identity、凭证隔离和请求预算。
原始 [矩阵](m5/android-matrix.json) 保留 run_id、API/ABI、APK 哈希与每项结果。

三并发 256 KiB 与 32 MiB 下载的 Debug 进程 PSS 峰值分别为 91,131 / 91,551 KiB，增长 420 KiB。
这是模拟器 Debug PSS 样本，不能当作 native RSS 或物理设备性能保证。
20 次取消到 release（含检查点）为 21～59 ms；样本见矩阵 JSON。

SIGKILL 与 am force-stop 均重建 provider，从 committed_offset=1024 精确 Range/If-Range 恢复，
最终 DigestVerified 并通过全文件 lease SHA-256。M5 另外完成正常模拟器 guest reboot，详见
[逐项进程/重启记录](m5/device-recovery.json)。进程测试不表示自动后台重启或掉电保证。

正式双 ABI AAR、完整依赖体积、外部 Maven Release/R8 宿主和符号归档在
[M5 报告](m5-baseline.md) 统一记录。测试 CA 仅在 sample Debug；AAR/Release 不含探针或信任锚。
使用独占 `adb reverse --no-rebind`，只清理本次拥有的映射；复用既有 AVD，不修改用户代理或停止其他设备。

缺测：API 24/目标 API 和其他 API、物理 arm64、实际 Android >4 GiB、真实网络切换、物理设备重启及掉电。
Linux 和 sanitizer 也未在本环境执行。这些门槛保持未通过，不能将提交标题中的“真机验证”作为证据。
