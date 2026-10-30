#!/usr/bin/env bash
set -euo pipefail

# Requires Linux, g++/gcc, Ninja, Python >=3.10, zlib/OpenSSL development files and CAs.
# Sanitizers run separately and are never installed in production packages.
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
bash "${task_root}/tools/ci/linux_m3.sh"
if [[ -n "${FETCHKIT_SANITIZERS:-}" ]]; then exit 0; fi
task_build="${task_root}/build/linux-m5-release"
task_prefix="${task_root}/build/linux-m5-install"
cmake -S "${task_root}" -B "${task_build}" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DFETCHKIT_WITH_CURL=ON -DFETCHKIT_USE_PINNED_CURL=ON -DFETCHKIT_INSTALL=ON \
    -DFETCHKIT_ENABLE_FAULT_INJECTION=OFF
cmake --build "${task_build}" --parallel 2
cmake --install "${task_build}" --prefix "${task_prefix}"
python3 "${task_root}/tools/ci/verify_native_package.py" --prefix "${task_prefix}" \
    --output "${task_root}/build/linux-m5-consumer"
python3 "${task_root}/tools/ci/run_m5_stress.py" --cli "${task_build}/fetchkit_cli" \
    --rounds "${FETCHKIT_STRESS_ROUNDS:-100}" --seed "${FETCHKIT_STRESS_SEED:-20261027}" \
    --output "${task_root}/build/linux-m5-stress"
# 部分托管 Runner 禁止普通用户写 uid_map；仅显式选中的 ENOSPC 实验使用特权控制器，
# 仍由驱动新建私有 user/mount namespace，不修改宿主 sysctl 或挂载布局。
task_enospc_runner=(python3)
if [[ "${FETCHKIT_ENOSPC_SUDO:-0}" == "1" ]]; then
    task_enospc_runner=(sudo -- /usr/bin/python3)
fi
"${task_enospc_runner[@]}" "${task_root}/tools/ci/run_m5_enospc.py" --cli "${task_build}/fetchkit_cli" \
    --output "${task_root}/build/linux-m5-enospc"
