#!/usr/bin/env python3
import plistlib
from pathlib import Path

root = Path(__file__).parent
makefile = (root / "Makefile").read_text()
preinstall = (root / "package-scripts" / "preinstall").read_text()
postinstall = (root / "package-scripts" / "postinstall").read_text()
agent = plistlib.loads((root / "launch-agents" / "io.github.maurojuniorr.numark-ns6-midi-bridge.plist").read_bytes())
components = plistlib.loads((root / "package-components.plist").read_bytes())

bridge_path = "/Applications/Numark NS6 Status.app/Contents/MacOS/NumarkNS6MIDIBridge"
assert bridge_path in agent["ProgramArguments"]
assert "$(APP)/Contents/MacOS/NumarkNS6MIDIBridge" in makefile
assert "$(APP) '$(PACKAGE_ROOT)/Applications/Numark NS6 Status.app'" in makefile
assert "launch-agents/io.github.maurojuniorr.numark-ns6-midi-bridge.plist $(PACKAGE_ROOT)/Library/LaunchAgents/" in makefile
assert any(
    component.get("RootRelativeBundlePath") == "Applications/Numark NS6 Status.app"
    and component.get("BundleOverwriteAction") == "upgrade"
    for component in components
)
assert "launchctl bootout" in preinstall
assert "launchctl bootstrap" in postinstall
assert "launchctl kickstart" in postinstall
