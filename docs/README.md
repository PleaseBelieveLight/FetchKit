# 文档中心

首次接入可按「[构建与运行](guides/development.md) → [Android API](reference/android-api.md) 或
[主机 API](reference/host-api.md)」阅读；已有产物的集成方直接查看 [接入与打包](release/packaging.md)。
当前为 `0.1.0-alpha.1`，支持本地预览和 GitHub Packages 发布配置，验收结果与限制以 [发布检查表](release/checklist.md) 和
[验证报告](reports/README.md) 为准。

## 接入与开发 · `guides/`

- [构建与运行](guides/development.md)：环境、工程结构、构建命令和验证入口。
- [Android 示例](guides/android-sample.md)：拾流 App 的下载、存储和后台任务操作。

## 接口与契约 · `reference/`

- [Android API](reference/android-api.md)：Kotlin/JNI、Future、生命周期、网络与后台执行。
- [主机 API](reference/host-api.md)：C/C++、调度、独立订阅、事件、重试和存储策略。
- [公共 API 与存储契约](reference/api-contract.md)：设计约束及实现演进，具体调用以以上 API 文档为准。
- [传输契约](reference/transport-contract.md)：响应接受、背压、取消和网络后端边界。
- [记录 v2](reference/storage-record-v2.md) / [记录 v1](reference/storage-record-v1.md)：当前持久格式与迁移基础。

## 架构与规划 · `design/`

- [首版设计基线](design/v1-design.md)：产品边界、核心架构、协议与验收场景。
- [首版落地计划](design/v1-implementation-plan.md)：工作包、依赖和阶段目标。

设计目标与历史阶段进展不代表当前能力已全部验收。

## 接入与发布 · `release/`

- [接入与打包](release/packaging.md)：CMake 安装、Maven AAR、GitHub Packages、CLI、符号与本地预览。
- [发布检查表](release/checklist.md)：必需证据和未完成门槛。
- [依赖与第三方许可](../third_party/README.md)：依赖锁定、构建与授权材料。

## 验证与历史

- [验证报告索引](reports/README.md)：M0～M5 结论、执行环境与原始证据，证据目录保持原路径。
- [历史文档索引](archive/README.md)：M1/M2 阶段接口，供追溯旧行为。
- [测试说明](../tests/README.md) / [体积测量](../tools/size/README.md)：与工具相邻的执行说明。
