#!/usr/bin/env python3
import plistlib
from pathlib import Path


ROOT = Path(__file__).parent
LABEL = "io.github.maurojuniorr.numark-ns6-device-service"
agent_path = ROOT / "launch-agents" / f"{LABEL}.plist"
agent = plistlib.loads(agent_path.read_bytes())
hal = plistlib.loads((ROOT / "Info.plist").read_bytes())
preinstall = (ROOT / "package-scripts-experimental" / "preinstall").read_text()
postinstall = (ROOT / "package-scripts-experimental" / "postinstall").read_text()
rollback = (ROOT / "rollback-persistent-service.sh").read_text()
makefile = (ROOT / "Makefile").read_text()
probe = (ROOT / "NS6DeviceServiceProbe.c").read_text()

assert agent["Label"] == LABEL
assert agent["KeepAlive"] is True and agent["RunAtLoad"] is True
assert LABEL in agent["MachServices"]
assert agent["ProgramArguments"][-1].endswith("/NumarkNS6DeviceService")
assert LABEL in hal["AudioServerPlugIn_MachServices"]

assert preinstall.index('if [[ "$CONSOLE_UID" == "0" ]]; then') < preinstall.index('/bin/rm -rf "$APP_SUPPORT"')
assert preinstall.index('ditto "$OLD_DRIVER"') < preinstall.index('rm -f "$OLD_AGENT"')
assert preinstall.index('cp "$OLD_AGENT"') < preinstall.index('rm -f "$OLD_AGENT"')
assert 'ditto "$STATUS_APP" "$APP_SUPPORT/Numark NS6 Status.app"' in preinstall
assert 'cp "$NEW_AGENT" "$APP_SUPPORT/numark-ns6-device-service.plist"' in preinstall
assert "launchctl bootout" in preinstall
assert postinstall.index('launchctl bootstrap') < postinstall.index('"$PROBE"') < postinstall.index('killall coreaudiod')
assert "restoring the previous installation" in postinstall
assert "ns6_device_client_get_state" in probe and "ns6_device_client_connect" in probe
assert '[[ -e "$APP_SUPPORT/had-driver" ]]' in rollback
assert '[[ -e "$APP_SUPPORT/had-status-app" ]]' in rollback
assert '[[ -e "$APP_SUPPORT/had-midi-agent" ]]' in rollback
assert '[[ -e "$APP_SUPPORT/had-device-service-agent" ]]' in rollback
assert "Run this rollback script with sudo" in rollback
assert "experimental-package" in makefile and "EXPERIMENTAL_VERSION" in makefile
assert "NS6DeviceServiceProbe" in makefile and "hal-mapping-test" in makefile

print("NS6 persistent-service install/rollback lifecycle checks passed")
