#include "AudioDeviceLifecycle.hpp"

#include <cassert>

int main() {
    AudioDeviceLifecycle device;

    // An input-only client and an output client share device lifetime.
    assert(device.startIO(101) == AudioIOTransition::FirstClientStarted);
    assert(device.startIO(202) == AudioIOTransition::AdditionalClientStarted);
    assert(device.stopIO(101) == AudioIOTransition::ClientStoppedStillActive);
    assert(device.activeStarts() == 1);
    assert(device.removeClient(202) == AudioIOTransition::LastClientStopped);
    assert(device.activeStarts() == 0);
    assert(device.stopIO(202) == AudioIOTransition::ClientNotActive);
}
