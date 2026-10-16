#ifndef NS6_AUDIO_TRANSPORT_POLICY_H
#define NS6_AUDIO_TRANSPORT_POLICY_H

#include <stdbool.h>

typedef enum {
    NS6_AUDIO_TRANSPORT_KEEP,
    NS6_AUDIO_TRANSPORT_START
} NS6AudioTransportAction;

static inline NS6AudioTransportAction ns6_audio_transport_action(bool transport_open, unsigned active_clients) {
    return active_clients && !transport_open ? NS6_AUDIO_TRANSPORT_START : NS6_AUDIO_TRANSPORT_KEEP;
}

#endif
