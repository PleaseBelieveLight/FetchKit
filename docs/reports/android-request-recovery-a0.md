# Android 请求恢复 A0 基线

[文档中心](../README.md) · [报告索引](README.md) · [A0 契约](../design/request-retry-contract.md)

日期：2026-11-02。基于提交 `97df1eb` 加本次 A0 改动，版本 `0.2.0-beta.3`。
本阶段固定契约和复现，未修复下载行为；A1 将基线断言转换为新请求成功恢复断言。

## 实际检查

- Windows x64，GNU C/C++ 16.1.0，Debug，无 curl、无故障注入的独立构建：
  `build/a0-request-recovery-20261102`。原 M0～M5 构建与证据未覆盖。
- 构建前通过 Windows GlobalMemoryStatusEx 读取可用物理内存 2.55 GiB / 15.72 GiB，单编译线程。
  CIM 查询不可用，改用 Windows 内存 API；未结束用户进程。
- 构建命令：`cmake --build build/a0-request-recovery-20261102 --target fetchkit_scheduler_test fetchkit_session_test --parallel 1`。
- 测试前可用物理内存 2.57 GiB。执行：
  `ctest --test-dir build/a0-request-recovery-20261102 --output-on-failure --no-tests=error -R '^(download_session|scheduler_storage)$' -j 1`。
- CTest **2/2 通过**，41.55 秒：download_session 5.59 秒（9 个会话），scheduler_storage 35.93 秒
  （原 21 组加 2 组特征场景；鉴权场景分别检查 401/403）。本地原始日志位于该构建的 `Testing/Temporary/LastTest.log`。
- Android 驱动 Python 语法与 `git diff --check` 通过；新增文档链接单独检查。

## 已复现的原因

1. `Failed + AuthRequired` 不满足现有恢复条件；刷新失败后新链接 enqueue 不启动，resume 返回 Busy。
   测试确认已提交的 1024 字节仍保留，缺口在执行准入而非数据丢失。
2. 共享任务选最早的有效订阅；暂停后追加新请求仍调用旧 provider，新 provider 调用次数为 0。

修复方向已固定为原子的新请求重试、显式本轮请求来源、保留普通预算与检查点，以及 provider/取消屏障。
见 A0 契约，不通过删除任务或重置预算绕过问题。

## 未执行项

Android 已增加两项正式 Kotlin/JNI 矩阵特征用例和驱动存在性门槛，但未构建/运行。
当前可用内存不足以同时为完整 Gradle/新模拟器留出合理余量；`adb devices` 也未能启动本机 daemon。
未启动新模拟器，未以主机测试声称 JNI 或网络后端通过。
资源充足且设备可用时，按测试指南运行 `python tools/ci/run_m4_android.py --serial <设备序列号> --output <新的证据目录>`。
Linux、sanitizer、真机、大文件和发布验收均未在 A0 执行。
