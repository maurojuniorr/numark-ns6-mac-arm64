#import <Foundation/Foundation.h>
#include <os/log.h>
#include "NS6USBActivity.h"

void *ns6_usb_activity_begin(void) {
    @autoreleasepool {
        /* USB continues in silence after the last CoreAudio client stops. */
        id token = [[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical
            reason:@"Numark NS6 continuous USB transport"];
        [token retain];
        os_log(OS_LOG_DEFAULT, "Numark NS6 USB latency activity begin: token available %d", token != nil);
        return (void *)token;
    }
}

void ns6_usb_activity_end(void *token) {
    if (!token) return;
    @autoreleasepool {
        [[NSProcessInfo processInfo] endActivity:(id)token];
        [(id)token release];
        os_log(OS_LOG_DEFAULT, "Numark NS6 USB latency activity ended");
    }
}
