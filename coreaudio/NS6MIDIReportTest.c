#include "NS6MIDI.c"
#include <assert.h>

static unsigned captured_reports;
static IOReturn capture_write(void *self, UInt8 pipe, void *data, UInt32 length) {
    (void)self;
    assert(pipe == 3);
    assert(length == 42);
    assert(((const unsigned char *)data)[41] == 0xe0);
    ++captured_reports;
    return kIOReturnSuccess;
}

int main(void) {
    const unsigned char led[] = {0xb1, 0x09, 0x7f};
    enqueue(led, sizeof(led));
    assert(tail == 1);
    assert(memcmp(queue[0].data, led, sizeof(led)) == 0);
    for (unsigned i = sizeof(led); i < 41; ++i) assert(queue[0].data[i] == 0xfd);
    assert(queue[0].data[41] == 0xe0);

    unsigned char payload[41];
    memset(payload, 0x40, sizeof(payload));
    payload[0] = 0xe0;
    enqueue(payload, sizeof(payload));
    assert(memcmp(queue[1].data, payload, sizeof(payload)) == 0);
    assert(queue[1].data[41] == 0xe0);
    unsigned char oversized[42] = {0};
    enqueue(oversized, sizeof(oversized));
    assert(tail == 2);

    IOUSBInterfaceInterface mock = {0};
    mock.WritePipe = capture_write;
    IOUSBInterfaceInterface *mock_pointer = &mock;
    usb = &mock_pointer;
    output_pipe = 3;
    blackout_controller();
    assert(captured_reports > 0);
    usb = NULL;
    puts("NS6 MIDI report tests passed (vendor OUT control byte, payload, padding, blackout)");
    return 0;
}
