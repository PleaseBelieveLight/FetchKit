#!/usr/bin/env bash
set -euo pipefail

# Same prerequisites as linux_m2.sh; Linux /proc is used for child memory metrics.
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
task_build="${task_root}/build/linux-m3"
task_sanitizers="${FETCHKIT_SANITIZERS:-}"
if [[ -n "${task_sanitizers}" ]]; then task_build="${task_build}-${task_sanitizers//,/-}"; fi
task_large=OFF
if [[ "${FETCHKIT_RUN_LARGE_FILE:-0}" == "1" ]]; then task_large=ON; fi
cmake -S "${task_root}" -B "${task_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFETCHKIT_WITH_CURL=ON -DFETCHKIT_USE_PINNED_CURL=ON \
    -DFETCHKIT_BUILD_TESTS=ON -DFETCHKIT_BUILD_PROBES=ON -DFETCHKIT_ENABLE_FAULT_INJECTION=ON \
    -DFETCHKIT_SANITIZERS="${task_sanitizers}" -DFETCHKIT_BUILD_LARGE_FILE_TESTS="${task_large}"
cmake --build "${task_build}" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
mkdir -p "${task_build}/reports"
ctest --test-dir "${task_build}" --output-on-failure --no-tests=error -LE large_file \
    --output-junit "${task_build}/reports/ctest.xml"
if [[ "${FETCHKIT_RUN_LARGE_FILE:-0}" == "1" ]]; then
    ctest --test-dir "${task_build}" --output-on-failure --no-tests=error -L large_file \
        --output-junit "${task_build}/reports/large-file-ctest.xml"
fi
