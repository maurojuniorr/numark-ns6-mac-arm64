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

assert agent["Label"] == LABEL
assert agent["KeepAlive"] is True and agent["RunAtLoad"] is True
assert LABEL in agent["MachServices"]
assert agent["ProgramArguments"][-1].endswith("/NumarkNS6DeviceService")
assert LABEL in hal["AudioServerPlugIn_MachServices"]

assert preinstall.index("ditto \"$OLD_DRIVER\"") < preinstall.index("rm -f \"$OLD_AGENT\"")
assert preinstall.index("cp \"$OLD_AGENT\"") < preinstall.index("rm -f \"$OLD_AGENT\"")
assert "launchctl bootout" in preinstall
assert postinstall.index("launchctl bootstrap") < postinstall.index("killall coreaudiod")
assert "state = running" in postinstall and "CoreAudio was left running" in postinstall
assert rollback.index("ditto \"$APP_SUPPORT/NumarkNS6M1.driver\"") < rollback.index("killall coreaudiod")
assert "Run this rollback script with sudo" in rollback
assert "experimental-package" in makefile and "EXPERIMENTAL_VERSION" in makefile

print("NS6 persistent-service install/rollback lifecycle checks passed")
