#!/usr/bin/env bash
set -euo pipefail
mkdir -p build/android-diagnostics
task_apk="$(find build/diagnostic-input -name sample-release.apk -print -quit)"
test -f "$task_apk"
adb -s emulator-5554 shell input keyevent KEYCODE_WAKEUP
adb -s emulator-5554 shell input keyevent KEYCODE_MENU
adb -s emulator-5554 install -r "$task_apk"
task_remote="$(adb -s emulator-5554 shell pm path com.luofelix.fetchkit | tr -d '\r' | sed -n 's/^package://p')"
printf '%s\n' "$task_remote" > build/android-diagnostics/apk-path.txt
adb -s emulator-5554 shell ls -l "$task_remote" > build/android-diagnostics/apk-mode.txt
if adb -s emulator-5554 pull "$task_remote" build/android-diagnostics/sync.apk > build/android-diagnostics/pull.txt 2>&1; then
    echo 'pull: success'
else
    echo "pull: failed $?, see pull.txt"
fi
adb -s emulator-5554 exec-out cat "$task_remote" > build/android-diagnostics/stream.apk
sha256sum "$task_apk" build/android-diagnostics/stream.apk > build/android-diagnostics/apk-hashes.txt
rm -- build/android-diagnostics/stream.apk
if [[ -f build/android-diagnostics/sync.apk ]]; then rm -- build/android-diagnostics/sync.apk; fi
adb -s emulator-5554 shell am start -W -n com.luofelix.fetchkit/com.luofelix.fetchkit.sample.MainActivity
sleep 5
adb -s emulator-5554 exec-out screencap -p > build/android-diagnostics/screen.png
adb -s emulator-5554 shell uiautomator dump /sdcard/fetchkit-diagnostic.xml
adb -s emulator-5554 exec-out cat /sdcard/fetchkit-diagnostic.xml > build/android-diagnostics/screen.xml
adb -s emulator-5554 exec-out uiautomator dump /dev/tty > build/android-diagnostics/tty.xml
adb -s emulator-5554 shell settings get global animator_duration_scale > build/android-diagnostics/animation-scale.txt
