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
assert "ns6_hal_mapping_acquire(&callback_mapping" in callback, "callbacks must pin a live, reclaim-safe shared mapping"
assert "ns6_hal_mapping_release(&callback_mapping)" in callback, "callbacks must release every shared mapping lease"
assert "static void *device_service_monitor(" in source, "service restarts must be detected outside the realtime callback"
assert "ns6_device_client_get_state(&device_client" in source, "the HAL monitor must validate the live Mach session"

print("HAL callback service boundary checks passed")
