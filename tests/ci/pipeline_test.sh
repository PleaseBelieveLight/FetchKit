#!/usr/bin/env bash

# 由工作流 source，保留实际 run 步骤的 shell 选项，不在测试内自行启用 pipefail。
# 复现失败命令经 tee 写日志的路径；若失败被吞掉，必须让本检查失败。
mkdir -p build/ci
if (false | tee build/ci/pipeline-guard.log); then
    echo 'CI 日志管道吞掉了失败状态；请显式使用启用 pipefail 的 Bash' >&2
    exit 1
fi
