#ifndef NS6_CAPTURE_DECODER_H
#define NS6_CAPTURE_DECODER_H

#include <stddef.h>
#include <stdint.h>

enum { NS6_CAPTURE_SOURCE_STRIDE=64, NS6_CAPTURE_CHANNELS=2 };

static inline float ns6_capture_decode_lane(const uint8_t *source){
    uint32_t value=0;
    for(size_t bit=0;bit<24;++bit)value=(value<<1)|(source[bit]&1u);
    int32_t signed_value=(value&0x800000u)?(int32_t)(value|0xff000000u):(int32_t)value;
    return (float)signed_value*(1.0f/8388608.0f);
}

static inline size_t ns6_capture_decode_block(const uint8_t *source,size_t source_bytes,float *output,size_t output_capacity_frames){
    if(!source||!output||source_bytes==0||source_bytes%NS6_CAPTURE_SOURCE_STRIDE!=0)return 0;
    size_t frames=source_bytes/NS6_CAPTURE_SOURCE_STRIDE;
    if(output_capacity_frames<frames)return 0;
    for(size_t frame=0;frame<frames;++frame){
        const uint8_t *sample=source+frame*NS6_CAPTURE_SOURCE_STRIDE;
        output[frame*NS6_CAPTURE_CHANNELS]=ns6_capture_decode_lane(sample);
        output[frame*NS6_CAPTURE_CHANNELS+1]=ns6_capture_decode_lane(sample+32);
    }
    return frames;
}

#endif
