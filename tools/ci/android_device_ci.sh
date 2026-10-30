#!/usr/bin/env bash
set -euo pipefail

# 仅供工作流新建的独占 AVD；签名切换需要卸载这个测试 guest 中刚安装的演示包。
# 不允许将该清理步骤用于本地已有设备，设备驱动本身仍保留原有不卸载约束。
if [[ "${GITHUB_ACTIONS:-}" != "true" ]] || [[ "${FETCHKIT_CI_AVD:-}" != "fetchkit-ci" ]]; then
    echo '该脚本仅允许操作 GitHub Actions 新建的 fetchkit-ci AVD' >&2
    exit 1
fi
task_serial=emulator-5554
test "$(adb -s "$task_serial" emu avd name | head -n 1 | tr -d '\r')" = fetchkit-ci
adb -s "$task_serial" shell input keyevent KEYCODE_WAKEUP
adb -s "$task_serial" shell input keyevent KEYCODE_MENU
python tools/ci/android_ci_config.py --output build/ci/android-config.env
task_ndk="$(sed -n 's/^ndk=//p' build/ci/android-config.env)"
task_build_tools="$(sed -n 's/^build-tools;//p' build/ci/android-toolchain.txt)"
"$ANDROID_HOME/build-tools/$task_build_tools/apksigner" verify --verbose --print-certs \
    platform/android/sample/build/outputs/apk/release/sample-release.apk > build/ci/apk-signature.txt

task_startup_flags=()
if [[ "$(adb -s "$task_serial" shell getprop ro.build.version.sdk | tr -d '\r')" == "24" ]]; then
    task_startup_flags+=(--emulator-root-pull)
fi
python tests/packaging/sample_release_startup_test.py --serial "$task_serial" "${task_startup_flags[@]}" \
    --apk platform/android/sample/build/outputs/apk/release/sample-release.apk --output build/ci/release-startup
python tools/ci/verify_android_package.py --repository build/m5-maven --sdk "$ANDROID_HOME" \
    --adb "$(command -v adb)" --serial "$task_serial" --abi x86_64 --java-home "$JAVA_HOME" \
    --output build/ci/aar-consumer

# Alpha 打包先匹配符号并核对许可/ABI，随后调试构建即使更新中间产物也不影响待分发包。
if [[ "${FETCHKIT_CI_PACKAGE:-0}" == "1" ]]; then
    python tools/release/package_preview.py --native-prefix build/alpha-install --native-build build/alpha-native \
        --repository build/m5-maven --dependencies build/ci/sdk-dependencies.txt \
        --ndk "$ANDROID_HOME/ndk/$task_ndk" --output build/alpha-preview
fi

adb -s "$task_serial" uninstall com.luofelix.fetchkit
python tools/ci/run_m4_android.py --serial "$task_serial" --skip-build --output build/ci/android-protocol
python tools/ci/run_m4_recovery.py --serial "$task_serial" --skip-build --output build/ci/android-recovery
