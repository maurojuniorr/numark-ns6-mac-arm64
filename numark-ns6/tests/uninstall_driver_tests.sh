#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
uninstaller="$project_dir/tools/uninstall-driver.sh"
test_root=$(mktemp -d /private/tmp/ns6-uninstall-test.XXXXXX)
trap 'rm -rf "$test_root"' EXIT HUP INT TERM

for path in \
  '/Library/Audio/Plug-Ins/HAL/NumarkNS6.driver/Contents/Info.plist' \
  '/Library/Audio/Plug-Ins/HAL/NumarkNS6M1.driver/Contents/Info.plist' \
  '/Library/Audio/Plug-Ins/HAL/NumarkNS6AudioOnly.driver/Contents/Info.plist' \
  '/Library/Application Support/Numark NS6/NumarkNS6MIDIBridge' \
  '/Library/Application Support/Numark NS6/NumarkNS6Status.tar.gz' \
  '/Library/LaunchAgents/io.github.maurojuniorr.numark-ns6-midi-bridge.plist' \
  '/Library/LaunchAgents/io.github.maurojuniorr.numark-ns6-audio-activator.plist' \
  '/Library/LaunchAgents/io.github.maurojuniorr.numark-ns6-audio-midi-bridge.plist' \
  '/Library/LaunchDaemons/io.github.maurojuniorr.numark-ns6-device-service.plist' \
  '/Applications/Numark NS6 Status.app/Contents/Info.plist'; do
  mkdir -p "$test_root$(dirname -- "$path")"
  : > "$test_root$path"
done

mkdir -p "$test_root/Library/Audio/Plug-Ins/HAL/Unrelated.driver" \
  "$test_root/Library/Preferences" "$test_root/Users/test/Library/Logs"
: > "$test_root/Library/Audio/Plug-Ins/HAL/Unrelated.driver/keep"
: > "$test_root/Library/Preferences/io.github.maurojuniorr.NumarkNS6.plist"
: > "$test_root/Users/test/Library/Logs/ns6.log"

NS6_UNINSTALL_TEST_ROOT="$test_root" "$uninstaller"

for path in \
  '/Library/Audio/Plug-Ins/HAL/NumarkNS6.driver' \
  '/Library/Audio/Plug-Ins/HAL/NumarkNS6M1.driver' \
  '/Library/Audio/Plug-Ins/HAL/NumarkNS6AudioOnly.driver' \
  '/Library/Application Support/Numark NS6' \
  '/Library/LaunchAgents/io.github.maurojuniorr.numark-ns6-midi-bridge.plist' \
  '/Library/LaunchAgents/io.github.maurojuniorr.numark-ns6-audio-activator.plist' \
  '/Library/LaunchAgents/io.github.maurojuniorr.numark-ns6-audio-midi-bridge.plist' \
  '/Library/LaunchDaemons/io.github.maurojuniorr.numark-ns6-device-service.plist' \
  '/Applications/Numark NS6 Status.app'; do
  if [ -e "$test_root$path" ]; then
    echo "expected removed: $path" >&2
    exit 1
  fi
done

test -e "$test_root/Library/Audio/Plug-Ins/HAL/Unrelated.driver/keep"
test -e "$test_root/Library/Preferences/io.github.maurojuniorr.NumarkNS6.plist"
test -e "$test_root/Users/test/Library/Logs/ns6.log"

if NS6_UNINSTALL_TEST_ROOT=/tmp "$uninstaller" >/dev/null 2>&1; then
  echo 'uninstaller accepted an unsafe test root' >&2
  exit 1
fi

echo 'uninstall test passed'
