#!/usr/bin/env python3
import plistlib
from pathlib import Path


ROOT = Path(__file__).parent
LABEL = "io.github.maurojuniorr.numark-ns6-device-service"
agent_path = ROOT / "launch-agents" / f"{LABEL}.plist"
midi_agent_path = ROOT / "launch-agents" / "io.github.maurojuniorr.numark-ns6-midi-bridge.plist"
midi_agent = plistlib.loads(midi_agent_path.read_bytes())
agent = plistlib.loads(agent_path.read_bytes())
hal = plistlib.loads((ROOT / "Info.plist").read_bytes())
preinstall = (ROOT / "package-scripts-experimental" / "preinstall").read_text()
postinstall = (ROOT / "package-scripts-experimental" / "postinstall").read_text()
rollback = (ROOT / "rollback-persistent-service.sh").read_text()
makefile = (ROOT / "Makefile").read_text()
probe = (ROOT / "NS6DeviceServiceProbe.c").read_text()
bridge_source = (ROOT / "NS6MIDIBridge.c").read_text()
usb_source = (ROOT / "NS6USB.c").read_text()
device_service_source = (ROOT / "NS6DeviceMachService.c").read_text()
component_plist = plistlib.loads((ROOT / "component-plist-experimental.plist").read_bytes())
system_daemon_path = ROOT / "launch-daemons" / f"{LABEL}.plist"
assert system_daemon_path.exists(), "persistent USB service must be a system LaunchDaemon visible to CoreAudio"
system_daemon = plistlib.loads(system_daemon_path.read_bytes())

assert agent["Label"] == LABEL
assert agent["KeepAlive"] is True and agent["RunAtLoad"] is True
assert LABEL in agent["MachServices"]
assert agent["ProgramArguments"][-1].endswith("/NumarkNS6DeviceService")
assert LABEL in hal["AudioServerPlugIn_MachServices"]

assert preinstall.index('ditto "$OLD_DRIVER"') < preinstall.index('rm -f "$OLD_AGENT"')
assert preinstall.index('cp "$OLD_AGENT"') < preinstall.index('rm -f "$OLD_AGENT"')
assert preinstall.index('cp "$SYSTEM_DAEMON"') < preinstall.index('launchctl bootout system')
assert 'NEW_AGENT' not in preinstall, "preinstall must not reference the removed NEW_AGENT variable"
assert 'cp "$OLD_DEVICE_AGENT" "$APP_SUPPORT/numark-ns6-device-service.plist"' in preinstall
assert 'ditto "$STATUS_APP" "$APP_SUPPORT/Numark NS6 Status.app"' in preinstall
assert 'cp "$OLD_DEVICE_AGENT" "$APP_SUPPORT/numark-ns6-device-service.plist"' in preinstall
assert "launchctl bootout" in preinstall
assert postinstall.index('launchctl bootstrap') < postinstall.index('"$PROBE"') < postinstall.index('killall coreaudiod')
assert "restoring the previous installation" in postinstall
assert "ns6_device_client_get_state" in probe and "ns6_device_client_connect" in probe
idle_audio_transition = device_service_source.split(
    "static void update_audio_active_from_clients(void) {", 1
)[1].split("static void log_active_clients", 1)[0]
assert "ns6_transport_discard()" not in idle_audio_transition, "idle must not discard buffers from another CoreAudio client"
paused_output = usb_source.split(
    "if(atomic_load_explicit(&output_paused,memory_order_acquire)", 1
)[1].split("if(atomic_load_explicit(&awaiting_audio_prefill", 1)[0]
assert "ns6_transport_available()==0" in paused_output, "idle silence should only replace output when no client audio is queued"
assert '[[ -e "$APP_SUPPORT/had-driver" ]]' in rollback
assert '[[ -e "$APP_SUPPORT/had-status-app" ]]' in rollback
assert '[[ -e "$APP_SUPPORT/had-midi-agent" ]]' in rollback
assert '[[ -e "$APP_SUPPORT/had-device-service-agent" ]]' in rollback
assert "Run this rollback script with sudo" in rollback
assert "experimental-package" in makefile and "EXPERIMENTAL_VERSION" in makefile
assert "NS6DeviceServiceProbe" in makefile and "hal-mapping-test" in makefile
assert "-DNS6_DEVICE_CLIENT_TRACE=1 NS6DeviceServiceProbe.c NS6DeviceClient.c" in makefile
assert system_daemon["Label"] == LABEL
assert system_daemon["KeepAlive"] is True and system_daemon["RunAtLoad"] is True
assert LABEL in system_daemon["MachServices"]
assert system_daemon["ProgramArguments"][-1].endswith("/NumarkNS6DeviceService")
assert 'launchctl bootstrap system "$DAEMON"' in postinstall
assert "Library/LaunchDaemons" in makefile and "Library/LaunchAgents" in makefile
assert "cp launch-agents/io.github.maurojuniorr.numark-ns6-midi-bridge.plist" in makefile
assert midi_agent["Label"] == "io.github.maurojuniorr.numark-ns6-midi-bridge"
assert 'launchctl bootstrap "gui/$CONSOLE_UID" "$MIDI_AGENT"' in postinstall
assert postinstall.index('launchctl bootstrap system') < postinstall.index('"$PROBE"') < postinstall.index('launchctl bootstrap "gui/$CONSOLE_UID"') < postinstall.index('killall coreaudiod')
service_main = bridge_source.split("int main(void){", 1)[1]
assert "#if NS6_DEVICE_SERVICE_BUILD" in service_main
service_entry = service_main.split("#else", 1)[0]
assert "start_device_service()" in service_entry and "MIDIClientCreate(" not in service_entry
assert "launchctl bootout system" in preinstall and "launchctl bootout system" in rollback
status_bundle = next(
    item for item in component_plist
    if item["RootRelativeBundlePath"] == "Applications/Numark NS6 Status.app"
)
assert status_bundle["BundleIsRelocatable"] is False
assert "--component-plist $(EXPERIMENTAL_COMPONENT_PLIST)" in makefile

print("NS6 persistent-service install/rollback lifecycle checks passed")
