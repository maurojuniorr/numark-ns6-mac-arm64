from pathlib import Path

source = Path(__file__).with_name("NS6HAL.c").read_text()
start = source.index("\nstatic OSStatus do_io(") + 1
end = source.index("static OSStatus end_io(", start)
callback = source[start:end]
write_mix_start = callback.index("if(operation==kAudioServerPlugInIOOperationWriteMix)")
write_mix = callback[write_mix_start:]

assert "ns6_transport_enqueue_shared(" in write_mix, "WriteMix must enqueue directly to the service shared ring"
for forbidden in (
    "ns6_usb_",
    "ns6_device_client_",
    "mach_msg(",
    "mach_vm_",
    "malloc(",
    "calloc(",
):
    assert forbidden not in callback, f"Audio callback must not call {forbidden}"
assert "ns6_capture_ring_read(&memory->capture" in callback, "ReadInput must consume the shared capture ring"
assert "memory->capture_requested" in callback, "ReadInput must publish capture demand to the USB service"

print("HAL callback service boundary checks passed")
