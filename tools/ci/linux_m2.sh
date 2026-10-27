#!/usr/bin/env bash
set -euo pipefail

# Linux prerequisites: C/C++17, CMake >=3.22.1, Ninja, Python >=3.10,
# libz development files, OpenSSL executable/development files and trusted CAs.
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
task_build="${task_root}/build/linux-m2"
task_sanitizers="${FETCHKIT_SANITIZERS:-}"
if [[ -n "${task_sanitizers}" ]]; then
    task_build="${task_root}/build/linux-m2-${task_sanitizers//,/-}"
fi
task_large=OFF
if [[ "${FETCHKIT_RUN_LARGE_FILE:-0}" == "1" ]]; then task_large=ON; fi
cmake -S "${task_root}" -B "${task_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFETCHKIT_WITH_CURL=ON -DFETCHKIT_USE_PINNED_CURL=ON \
    -DFETCHKIT_BUILD_TESTS=ON -DFETCHKIT_BUILD_PROBES=ON -DFETCHKIT_ENABLE_FAULT_INJECTION=ON \
    -DFETCHKIT_SANITIZERS="${task_sanitizers}" -DFETCHKIT_BUILD_LARGE_FILE_TESTS="${task_large}"
cmake --build "${task_build}" --parallel
ctest --test-dir "${task_build}" --output-on-failure -LE large_file
if [[ "${FETCHKIT_RUN_LARGE_FILE:-0}" == "1" ]]; then
    ctest --test-dir "${task_build}" --output-on-failure --no-tests=error -L large_file
fi
