#!/bin/zsh
set -eu
APP_SUPPORT="/Library/Application Support/Numark NS6/rollback-previous"
NEW_AGENT="/Library/LaunchAgents/io.github.maurojuniorr.numark-ns6-device-service.plist"
SYSTEM_DAEMON="/Library/LaunchDaemons/io.github.maurojuniorr.numark-ns6-device-service.plist"
OLD_AGENT="/Library/LaunchAgents/io.github.maurojuniorr.numark-ns6-midi-bridge.plist"
DRIVER="/Library/Audio/Plug-Ins/HAL/NumarkNS6M1.driver"
STATUS_APP="/Applications/Numark NS6 Status.app"
CONSOLE_UID=$(/usr/bin/stat -f '%u' /dev/console)

if [[ $EUID -ne 0 ]]; then
    print -u2 'Run this rollback script with sudo.'
    exit 1
fi
if [[ ! -d "$APP_SUPPORT" ]]; then
    print -u2 "No installation snapshot found in $APP_SUPPORT; rollback cannot continue."
    exit 1
fi

/bin/launchctl bootout system "$SYSTEM_DAEMON" >/dev/null 2>&1 || true
if [[ "$CONSOLE_UID" != "0" ]]; then
    /bin/launchctl bootout "gui/$CONSOLE_UID" "$NEW_AGENT" >/dev/null 2>&1 || true
    /bin/launchctl bootout "gui/$CONSOLE_UID" "$OLD_AGENT" >/dev/null 2>&1 || true
fi
/usr/bin/killall NumarkNS6DeviceService NumarkNS6MIDIBridge >/dev/null 2>&1 || true
/usr/bin/killall -m '^Core Audio Driver \(NumarkNS6M1\.driver\)$' >/dev/null 2>&1 || true

/bin/rm -rf "$DRIVER" "$STATUS_APP"
/bin/rm -f "$NEW_AGENT" "$OLD_AGENT" "$SYSTEM_DAEMON"
if [[ -e "$APP_SUPPORT/had-driver" ]]; then
    /usr/bin/ditto "$APP_SUPPORT/NumarkNS6M1.driver" "$DRIVER"
fi
if [[ -e "$APP_SUPPORT/had-status-app" ]]; then
    /usr/bin/ditto "$APP_SUPPORT/Numark NS6 Status.app" "$STATUS_APP"
fi
if [[ -e "$APP_SUPPORT/had-midi-agent" ]]; then
    /bin/cp "$APP_SUPPORT/numark-ns6-midi-bridge.plist" "$OLD_AGENT"
    /bin/launchctl bootstrap "gui/$CONSOLE_UID" "$OLD_AGENT" >/dev/null 2>&1 || true
fi
if [[ -e "$APP_SUPPORT/had-device-service-agent" ]]; then
    /bin/cp "$APP_SUPPORT/numark-ns6-device-service.plist" "$NEW_AGENT"
    /bin/launchctl bootstrap "gui/$CONSOLE_UID" "$NEW_AGENT" >/dev/null 2>&1 || true
fi
if [[ -e "$APP_SUPPORT/had-device-service-daemon" ]]; then
    /bin/cp "$APP_SUPPORT/numark-ns6-device-service-daemon.plist" "$SYSTEM_DAEMON"
    /bin/launchctl bootstrap system "$SYSTEM_DAEMON" >/dev/null 2>&1 || true
fi
/usr/bin/killall coreaudiod >/dev/null 2>&1 || true
print 'Restored the complete saved NS6 installation snapshot.'
