from pathlib import Path


source = Path("NS6MIDI.c").read_text(encoding="utf-8")
worker = source.split("static void *write_worker(void *unused){", 1)[1]
recovery = worker.split("bool all_submitted=recovered;", 1)[0]
clear_path = recovery.split("if(clear_pending){", 1)[1]

abort = clear_path.index("(*usb)->AbortPipe(usb,input_pipe)")
drain = clear_path.index("wait_for_input_reads_to_drain()")
clear = clear_path.index('recover_stalled_pipe(input_pipe,"input")')

assert abort < drain < clear, "MIDI recovery must abort, drain callbacks, then clear the endpoint stall"
print("NS6 MIDI recovery abort/drain/clear ordering test passed")
