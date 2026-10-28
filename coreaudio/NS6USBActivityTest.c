#include "NS6USBActivity.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    ns6_usb_activity_end(NULL);
    void *first = ns6_usb_activity_begin();
    void *second = ns6_usb_activity_begin();
    assert(first && second);
    ns6_usb_activity_end(first);
    ns6_usb_activity_end(second);
    puts("USB activity acquire/release smoke test passed");
    return 0;
}
