#include "NS6DeviceClient.h"

#include <stdio.h>

int main(void) {
    NS6DeviceClient client = {.service_port = MACH_PORT_NULL};
    NS6DeviceStateSnapshot state = {0};
    if (!ns6_device_client_connect(&client)) {
        fputs("NS6 Mach service handshake failed\n", stderr);
        return 1;
    }
    bool ready = ns6_device_client_get_state(&client, &state);
    if (ready)
        printf("NS6 Mach service ready (USB %s, transport %s)\n",
               state.connected ? "connected" : "disconnected",
               state.transport_ready ? "ready" : "waiting");
    ns6_device_client_disconnect(&client);
    if (!ready) fputs("NS6 Mach service state query failed\n", stderr);
    return ready ? 0 : 1;
}
