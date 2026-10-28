from pathlib import Path

source = Path("NS6MIDI.c").read_text(encoding="utf-8")
pipe_scan = source.split("static bool find_pipes(void){", 1)[1].split("static void enqueue(", 1)[0]

assert "GetPipeProperties" in pipe_scan
assert "direction==kUSBIn?0x80u:0u" in pipe_scan
assert "endpoint 0x%02x" in pipe_scan
assert "direction %u" in pipe_scan
assert "type %u" in pipe_scan
assert "max packet %u" in pipe_scan
assert "interval %u" in pipe_scan
print("NS6 MIDI pipe descriptor diagnostics test passed")
