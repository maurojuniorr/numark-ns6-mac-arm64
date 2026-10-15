#include "NS6CaptureDecoder.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static void encode_sample(uint8_t *block, size_t lane_offset, uint32_t value){
    for(size_t bit=0;bit<24;++bit)
        block[lane_offset+bit]=(uint8_t)((value>>(23-bit))&1u);
}

int main(void){
    uint8_t block[10240]={0};
    float output[160*2]={0};
    encode_sample(block,0,0x400000u);
    encode_sample(block,32,0xc00000u);
    size_t frames=ns6_capture_decode_block(block,sizeof(block),output,160);
    assert(frames==160);
    assert(output[0]>0.49f&&output[0]<0.51f);
    assert(output[1]<-0.49f&&output[1]>-0.51f);
    for(size_t frame=1;frame<frames;++frame){
        assert(output[frame*2]==0.0f);
        assert(output[frame*2+1]==0.0f);
    }
    assert(ns6_capture_decode_block(block,63,output,160)==0);
    assert(ns6_capture_decode_block(block,sizeof(block),output,159)==0);
    return 0;
}
