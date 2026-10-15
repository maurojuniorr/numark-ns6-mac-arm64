#include <assert.h>
#include <stdint.h>

#include "NS6AudioPrefill.h"

int main(void){
    assert(ns6_audio_startup_prefill_frames(256,1024,4096)==4096);
    assert(ns6_audio_startup_prefill_frames(8192,1024,4096)==8192);
    assert(ns6_audio_startup_prefill_frames(256,1024,512)==1024);
    return 0;
}
