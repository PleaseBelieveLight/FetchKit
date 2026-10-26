#!/usr/bin/env bash
set -euo pipefail

# Prerequisites: C/C++17 compiler, CMake >=3.22.1, Ninja, Python >=3.10,
# OpenSSL executable + development headers, working system CA certificates.
# Run from a Linux checkout. No packages are installed and no system settings change.
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
task_build="${task_root}/build/linux-m1"
task_large=OFF
if [[ "${FETCHKIT_RUN_LARGE_FILE:-0}" == "1" ]]; then task_large=ON; fi
cmake -S "${task_root}" -B "${task_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DFETCHKIT_WITH_CURL=ON -DFETCHKIT_USE_PINNED_CURL=ON \
    -DFETCHKIT_BUILD_TESTS=ON -DFETCHKIT_BUILD_PROBES=ON -DFETCHKIT_BUILD_LARGE_FILE_TESTS="${task_large}"
cmake --build "${task_build}" --parallel
ctest --test-dir "${task_build}" --output-on-failure -LE large_file
if [[ "${FETCHKIT_RUN_LARGE_FILE:-0}" == "1" ]]; then
    ctest --test-dir "${task_build}" --output-on-failure --no-tests=error -L large_file
fi
