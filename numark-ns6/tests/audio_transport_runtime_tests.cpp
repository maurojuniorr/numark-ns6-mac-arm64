#include "../coreaudio/AudioOutputEngine.hpp"
#include "../coreaudio/AudioTransportRuntime.hpp"

#include <cassert>

namespace {
}

int main() {
    NS6DeviceSession session;
    AudioOutputEngine engine(32);
    AudioInputEngine input(32, AudioInputReader::kTransferBytes /
                                AudioInputDecoder::kSourceStrideBytes);
    AudioTransportRuntime runtime(session, engine.ring(), input);
    assert(&runtime.outputRing() == &engine.ring());
    assert(runtime.start() == kIOReturnNotReady);
    assert(runtime.stop() == kIOReturnSuccess);
}
