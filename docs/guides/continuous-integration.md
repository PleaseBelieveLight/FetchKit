# GitHub Actions 持续集成与 Alpha 发布

[文档中心](../README.md) · [测试说明](../../tests/README.md) · [接入与打包](../release/packaging.md)

## 工作流

| 工作流 | 触发方式 | 验证范围 |
|--------|----------|----------|
| `CI` | `master`/`main` push、全部 PR、手动 | Linux curl 开/关两种主机配置；Android API 31 集成 |
| `Android integration` | CI/扩展验证复用，或手动选择 API 24/31/37 | 双 ABI Debug/Release 构建、Debug 单测、发布拒绝路径、实际 Release APK 冷启动、外部 Maven/R8/JNI 下载、Android 协议与进程恢复 |
| `Extended validation` | 每周一北京时间 03:17、手动 | Linux 实际 >4 GiB、生产安装/外部接入、固定 seed 压力、真实 ENOSPC；独立 ASan/UBSan 和 TSan；API 24/31/37 x86_64 模拟器矩阵 |
| `Alpha release` | 从 Alpha Tag 手动运行 | 固定提交重新执行主机/Android 回归、检查包内容/许可/双 ABI/匹配符号，生成校验值；可选上传 Packages 并创建草稿预发布 |

定义位于 [`.github/workflows/`](../../.github/workflows/)。第三方 Action 固定到提交 SHA，更新时一并核对上游说明。
CI 只有 `contents: read`，不使用仓库发布凭证，不自动评论 PR；新提交取消同一 PR/分支的旧 CI。
Alpha 发布 Job 单独拥有 `contents: write`、`packages: write`，令牌只在远程发布步骤传给 Gradle/gh。

Android 环境读取 `gradle/gradle-daemon-jvm.properties`、`gradle/libs.versions.toml` 和两个模块的
`compileSdk`；安装完整 Temurin JDK、对应正式 SDK/build-tools、NDK 和 CMake。SDK 包名依据
`sdkmanager --list` 选择，兼容 `android-37.0` 等平台命名。不会自动升级仓库依赖。
工作流显式将 SDK 的 `cmdline-tools/latest/bin` 和 `platform-tools` 加入当前及后续步骤的 PATH，
不依赖 Runner 是否预先导出这些路径。
Gradle 使用仓库 Wrapper，缓存采用 GitHub Actions 基础缓存；PR/Tag 仅读缓存，发布禁用配置缓存。

模拟器工作流使用明确的 Pixel 6 手机硬件配置，新建独占的 `fetchkit-ci` AVD，不使用快照。
避免依赖 AVD 默认的低分辨率通用设备配置；Release 页面检查仍要求实际按钮文字与页面入口完整可用。
保留正常系统动画，真实 Release 的 Compose 动画和页面可访问性节点也属于启动回归范围。
先验证公开演示签名的真实 Release APK，再验证
外部 AAR；随后仅卸载该新建 guest 中的演示包以切换 Debug 签名，执行正式 SDK 协议与 SIGKILL/force-stop 恢复。
`android_device_ci.sh` 同时检查 Actions 标识和 AVD 名，不允许直接用于本地已有设备。
API 24 的 userdebug guest 若禁止读取已安装 APK，仅在读取时临时 root adbd，立即恢复后再启动应用；
保留与候选 APK 的完整 SHA-256 比对，报告标记该读取方式，不对真机启用此选项。

## 日志与门槛

每次运行上传独立 Artifact，保留工作流 run/attempt、源码提交、环境与原始 JSON/XML/日志的 SHA-256。
普通结果保留 14 天，扩展验证与 Alpha 候选保留 30 天；大文件内容、下载缓存和用户凭证不归档。
CTest 启用 `--no-tests=error` 并生成 JUnit；Android 检查两个模块 Debug 单测的实际类/用例数，
缺失、全部跳过或失败均不能通过。设备测试继续检查现有驱动的 run_id、JUnit 输出和内容断言。
所有工作流显式选择 Bash，使 `run` 步骤启用 `-e -o pipefail`；经 `tee` 保存日志的构建或测试失败
必须保留非零状态，不能只根据日志上传成功判断验收通过。

`collect_ci_evidence.py` 为当前 CI 单独归档，保留历史 `collect_m5_evidence.py` 和 `docs/reports/` 原样。
`run.json` 的 `job_status` 仅表示本 Job 结果，`release_acceptance` 固定为 false。
它不能直接充当 `check_release.py` 所需的正式发布证据：需从同一候选产物的真实执行记录补齐命令、输入指纹、
平台和原始日志，按[发布检查表](../release/checklist.md)生成对应汇总。

Linux ENOSPC 依赖用户/挂载命名空间；Runner 禁止时工作流失败，不静默跳过或改用注入结果。
sanitizer 运行时或模拟器镜像不可用也按失败报告。托管模拟器不覆盖物理 arm64、物理重启/掉电；
正式发布仍须独立完成所有门槛。扩展任务不安装 sanitizer 或故障注入产物。

工作流合入并在 GitHub 实际跑通后，可在仓库 Rulesets/分支保护中将 `CI` 下的两个 Native 检查和 Android
检查设为合并必需项；以首次运行显示的检查名为准。配置文件不会自动修改仓库设置。

## 手动准备和分发 Alpha

1. 先将工作流合入默认分支。递增根目录 `VERSION` 的 Alpha 序号，提交全部候选源码，再创建并推送
   `v<VERSION>` Tag。Tag 必须包含这些工作流；保留已经公开的旧 Tag，不移动它们。
2. 在 Actions 的 `Alpha release` 中选择该 Tag 运行，`publish` 保持 false。也可使用 GitHub CLI：

   ```sh
   gh workflow run alpha-release.yml --ref v0.1.0-alpha.2 -F publish=false
   ```

   此处版本仅示例，必须与候选提交的 `VERSION` 一致。从分支、其他版本 Tag、Beta/RC 或正式版运行会失败。
3. 下载 Artifact 检查 AAR、sources、POM/module、Native 预览、sample APK、symbols/licenses、
   `SHA256SUMS`、manifest 和测试证据。APK/AAR 指纹必须与真实启动/外部接入记录一致；源码工作区必须干净。
4. 确认 Alpha 分发范围后，从同一 Tag 再次手动运行并选择 `publish=true`。工作流重新构建和验证该候选，
   创建 GitHub 草稿预发布，执行现有 GitHub Packages 发布任务，再上传附件。使用本仓库的 `GITHUB_TOKEN`，
   不需要额外保存 PAT。上传步骤复用本次已验证的 AAR/sources/POM/module，排除重新生成这些文件的任务；
   发布任务自身的探针、ABI 和凭证检查继续执行。最后由所有者检查草稿并公开。

准备模式只上传 Actions Artifact。远程步骤不是事务：失败时可能留下草稿或已经写入的 Maven 文件。
检查远程状态后人工处理，不通过移动 Tag、覆盖产物或盲目重跑发布来掩盖失败；不同源码使用新版本。
GitHub Packages 的使用方认证要求仍见[打包指南](../release/packaging.md#github-packages-发布与接入)。

Alpha 流程不运行正式发布检查器的通过路径，不声明首版发布验收完成；正式版仍由
`tools/release/check_release.py` 严格阻断缺失证据的分发。

## 本地检查 CI 工具

使用 Python 3.11+（CI 为 3.12）执行以下检查，无需发布凭证或设备：

```sh
python -m unittest discover -s tests/ci -p '*_test.py' -v
python tools/ci/android_ci_config.py
actionlint -shellcheck=""
bash -n tools/ci/android_device_ci.sh tools/ci/linux_m3.sh
git diff --check
```

`actionlint` 是维护工作流时独立使用的校验工具，不是 SDK 依赖。
工具单测覆盖 Tag/提交不匹配、错误版本、未验证或更换的产物、SDK 包缺失、空/跳过/失败的 JUnit 结果。
完整构建、设备回归和 Linux 专项按上表在相应环境执行；本地语法检查不能代替 Runner 结果。
