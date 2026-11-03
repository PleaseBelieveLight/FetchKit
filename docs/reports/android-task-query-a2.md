# Android 单任务查询 A2 验证

[文档中心](../README.md) · [报告索引](README.md) · [实施计划](../design/android-sdk-completeness-plan.md)

2026-11-03，基于 A1 提交 `dd2dfea`。C、C++ 和 Kotlin 均支持按持久 ID 或资源身份查询。
按身份查询复用资源 ID 计算，两个入口直接查资源索引并合并运行快照。列表与单条查询共用快照装配。
示例按需刷新详情并在打开重新绑定面板前取得当前快照。

- Windows x64 主机 Debug 构建。过滤 CTest 的 `c_api` 和 `scheduler_storage` 通过。
  新增用例覆盖 UTF-8 身份、非法 ID、关闭后查询、重开、资源过期及与删除、淘汰和关闭的竞争。
- SDK、sample Debug 和测试 APK 构建通过。SDK 15 项和 sample 4 项 JVM 单测通过。
- API 31 x86_64 模拟器的正式 JNI/OkHttp 下载矩阵 84/84，受控 HTTP 请求 187 次。
  新用例覆盖补充平面字符、运行中快照、暂停并解除订阅后的检查点，以及删除后的 NotFound。

本地日志位于 `build/a2-query-20261103/`。设备报告为 `device/report.json`，run_id 为
`9dac63a6-9892-4fee-8326-8048a15c7eff`。Gradle 原始输出见 `gradle.log`。
构建前分别读取可用物理内存，限制为一个 worker。设备验证使用现有 1 GiB AVD，结束后关闭本次启动的实例。
真机、Linux、其他 API 和 Release/R8 留待后续验收。
