#!/bin/sh
set -eu

test_root=${NS6_UNINSTALL_TEST_ROOT:-}
if [ -n "$test_root" ]; then
  case "$test_root" in
    /private/tmp/ns6-uninstall-test.*) ;;
    *) echo "Refusing unsafe test root: $test_root" >&2; exit 2 ;;
  esac
  [ -d "$test_root" ] || { echo "Test root does not exist." >&2; exit 2; }
  test_root=$(CDPATH= cd -- "$test_root" && pwd -P)
  case "$test_root" in
    /private/tmp/ns6-uninstall-test.*) ;;
    *) echo "Refusing unsafe test root: $test_root" >&2; exit 2 ;;
  esac
else
  if [ "$(/usr/bin/id -u)" -ne 0 ]; then
    echo "Run with administrator privileges: sudo $0" >&2
    exit 1
  fi
  printf 'This removes the Numark NS6 driver, MIDI bridge, Status app, and old helpers, then restarts CoreAudio. Audio will stop briefly. Continue? [y/N] '
  IFS= read -r answer || answer=
  case "$answer" in
    y|Y|yes|YES) ;;
    *) echo "Cancelled."; exit 1 ;;
  esac

  uid=$(/usr/bin/stat -f '%u' /dev/console)
  if [ "$uid" -gt 0 ]; then
    for label in \
      io.github.maurojuniorr.numark-ns6-midi-bridge \
      io.github.maurojuniorr.numark-ns6-audio-activator \
      io.github.maurojuniorr.numark-ns6-audio-midi-bridge; do
      /bin/launchctl bootout "gui/$uid/$label" >/dev/null 2>&1 || true
    done
  fi
  /bin/launchctl bootout system/io.github.maurojuniorr.numark-ns6-device-service \
    >/dev/null 2>&1 || true
  /usr/bin/killall NumarkNS6DeviceService NumarkNS6MIDIBridge \
    NumarkNS6AudioActivator NumarkNS6Status >/dev/null 2>&1 || true
  /usr/bin/pkill -f '^Core Audio Driver \(NumarkNS6\.driver\)$' \
    >/dev/null 2>&1 || true
fi

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
  /bin/rm -rf "$test_root$path"
done

if [ -z "$test_root" ]; then
  for receipt in \
    io.github.maurojuniorr.NumarkNS6 \
    io.github.maurojuniorr.numark-ns6-audio-only \
    io.github.maurojuniorr.numark-ns6.experimental; do
    /usr/sbin/pkgutil --forget "$receipt" >/dev/null 2>&1 || true
  done
  /usr/bin/killall coreaudiod >/dev/null 2>&1 || true
  echo 'Numark NS6 components removed. CoreAudio is restarting.'
fi
