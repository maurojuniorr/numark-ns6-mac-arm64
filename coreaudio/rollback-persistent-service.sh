#!/bin/zsh
set -eu
APP_SUPPORT="/Library/Application Support/Numark NS6/rollback-previous"
NEW_AGENT="/Library/LaunchAgents/io.github.maurojuniorr.numark-ns6-device-service.plist"
OLD_AGENT="/Library/LaunchAgents/io.github.maurojuniorr.numark-ns6-midi-bridge.plist"
DRIVER="/Library/Audio/Plug-Ins/HAL/NumarkNS6M1.driver"
CONSOLE_UID=$(/usr/bin/stat -f '%u' /dev/console)

if [[ $EUID -ne 0 ]]; then
    print -u2 'Run this rollback script with sudo.'
    exit 1
fi
if [[ ! -d "$APP_SUPPORT/NumarkNS6M1.driver" ]]; then
    print -u2 "No saved driver found in $APP_SUPPORT; rollback cannot continue."
    exit 1
fi
if [[ "$CONSOLE_UID" != "0" ]]; then
    /bin/launchctl bootout "gui/$CONSOLE_UID" "$NEW_AGENT" >/dev/null 2>&1 || true
fi
/usr/bin/killall NumarkNS6DeviceService >/dev/null 2>&1 || true
/usr/bin/killall -m '^Core Audio Driver \(NumarkNS6M1\.driver\)$' >/dev/null 2>&1 || true
/bin/rm -rf "$DRIVER"
/usr/bin/ditto "$APP_SUPPORT/NumarkNS6M1.driver" "$DRIVER"
/bin/rm -f "$NEW_AGENT"
if [[ -f "$APP_SUPPORT/numark-ns6-midi-bridge.plist" ]]; then
    /bin/cp "$APP_SUPPORT/numark-ns6-midi-bridge.plist" "$OLD_AGENT"
    if [[ "$CONSOLE_UID" != "0" ]]; then
        /bin/launchctl bootstrap "gui/$CONSOLE_UID" "$OLD_AGENT" >/dev/null 2>&1 || true
    fi
fi
/usr/bin/killall coreaudiod >/dev/null 2>&1 || true
print 'Restored the saved HAL driver and MIDI bridge LaunchAgent.'
