from pathlib import Path
source = Path(__file__).with_name("NS6HAL.c").read_text()
start = source.index("\nstatic OSStatus do_io(") + 1
end = source.index("static OSStatus end_io(", start)
callback = source[start:end]
will_do_start = source.index("static OSStatus will_do(AudioServerPlugInDriverRef d,")
will_do = source[will_do_start:source.index("static OSStatus begin_io(AudioServerPlugInDriverRef d,", will_do_start)]
assert "*will=operation==kAudioServerPlugInIOOperationWriteMix||operation==kAudioServerPlugInIOOperationReadInput" in will_do, "the HAL must use the established per-client WriteMix path"
assert "kAudioServerPlugInIOOperationMixOutput" not in will_do, "MixOutput must not be negotiated while blocks are appended to a FIFO"
output_start = callback.index("if(operation==kAudioServerPlugInIOOperationWriteMix)")
output_mix = callback[output_start:]

assert "ns6_transport_enqueue_shared(output_ring,main_buffer,frames,&stream_format)" in output_mix, "CoreAudio output blocks must use the established arrival-order FIFO"
assert "ns6_transport_mix_at_shared" not in output_mix, "CoreAudio output must not depend on per-client timestamp alignment"
assert "ns6_io_metrics_record_output_timed(&ns6_io_metrics,client,frames" in output_mix, "WriteMix must keep per-client output timing telemetry"
assert "kAudioTimeStampSampleTimeValid" in output_mix, "WriteMix telemetry must retain CoreAudio's sample timeline"
assert "AudioConvertHostTimeToNanos(i->mOutputTime.mHostTime)" not in output_mix, "absolute host time must not be mistaken for a device sample coordinate"
assert "output buffer: %u cumulative failures, client %u" in output_mix, "failed FIFO writes must retain a client-scoped diagnostic"
enqueue_failure = output_mix[output_mix.index("if(!accepted){"):]
assert "return kAudioHardwareUnspecifiedError" not in enqueue_failure, "a dropped output block must not fail the CoreAudio I/O cycle"
assert "A dropped block must not tear down every CoreAudio client." in enqueue_failure and "return 0;" in enqueue_failure, "a rejected block must be dropped and logged without failing the device"
assert "kAudioServerPlugInIOOperationMixOutput" not in output_mix, "the FIFO callback must not append MixOutput client blocks"
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
assert "duplicate cycles %llu" in source, "output diagnostics must report duplicate CoreAudio cycles"
assert "skipped cycles %llu" in source, "output diagnostics must report skipped CoreAudio cycles"
assert "last cycle %llu" in source, "output diagnostics must report the last CoreAudio cycle"
assert "StartIO lock wait %llu us" in source, "StartIO diagnostics must expose mutex wait time"
assert "StartIO total %llu us" in source, "StartIO diagnostics must expose total duration"

print("HAL callback service boundary checks passed")
