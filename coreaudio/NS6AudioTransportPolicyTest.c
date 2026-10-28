#include "NS6AudioTransportPolicy.h"
#include <assert.h>

int main(void) {
    assert(ns6_audio_transport_action(false, 0) == NS6_AUDIO_TRANSPORT_KEEP);
    assert(ns6_audio_transport_action(false, 1) == NS6_AUDIO_TRANSPORT_START);
    assert(ns6_audio_transport_action(true, 1) == NS6_AUDIO_TRANSPORT_KEEP);
    assert(ns6_audio_transport_action(true, 2) == NS6_AUDIO_TRANSPORT_KEEP);
    assert(ns6_audio_transport_action(true, 1) == NS6_AUDIO_TRANSPORT_KEEP);
    assert(ns6_audio_transport_action(true, 0) == NS6_AUDIO_TRANSPORT_KEEP);
    assert(ns6_audio_transport_action(true, 1) == NS6_AUDIO_TRANSPORT_KEEP);
    return 0;
}
