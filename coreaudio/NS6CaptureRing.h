#ifndef NS6_CAPTURE_RING_H
#define NS6_CAPTURE_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

enum { NS6_CAPTURE_RING_CAPACITY=8192, NS6_CAPTURE_RING_CHANNELS=2 };

typedef struct {
    float samples[NS6_CAPTURE_RING_CAPACITY*NS6_CAPTURE_RING_CHANNELS];
    atomic_uint_fast64_t head;
    atomic_uint_fast64_t tail;
} NS6CaptureRing;

static inline void ns6_capture_ring_init(NS6CaptureRing *ring){
    if(!ring)return;
    atomic_init(&ring->head,0);
    atomic_init(&ring->tail,0);
}

static inline void ns6_capture_ring_clear(NS6CaptureRing *ring){
    if(!ring)return;
    uint64_t head=atomic_load_explicit(&ring->head,memory_order_acquire);
    atomic_store_explicit(&ring->tail,head,memory_order_release);
}

static inline size_t ns6_capture_ring_write(NS6CaptureRing *ring,const float *input,size_t frames){
    if(!ring||!input||frames==0)return 0;
    uint64_t head=atomic_load_explicit(&ring->head,memory_order_relaxed);
    uint64_t tail=atomic_load_explicit(&ring->tail,memory_order_acquire);
    uint64_t used=head-tail;
    if(used>=NS6_CAPTURE_RING_CAPACITY)return 0;
    size_t available=NS6_CAPTURE_RING_CAPACITY-(size_t)used;
    if(frames>available)frames=available;
    for(size_t frame=0;frame<frames;++frame){
        size_t slot=(size_t)((head+frame)%NS6_CAPTURE_RING_CAPACITY)*NS6_CAPTURE_RING_CHANNELS;
        ring->samples[slot]=input[frame*NS6_CAPTURE_RING_CHANNELS];
        ring->samples[slot+1]=input[frame*NS6_CAPTURE_RING_CHANNELS+1];
    }
    atomic_store_explicit(&ring->head,head+frames,memory_order_release);
    return frames;
}

static inline size_t ns6_capture_ring_read(NS6CaptureRing *ring,float *output,size_t frames){
    if(!ring||!output||frames==0)return 0;
    uint64_t tail=atomic_load_explicit(&ring->tail,memory_order_relaxed);
    uint64_t head=atomic_load_explicit(&ring->head,memory_order_acquire);
    uint64_t available=head-tail;
    if(available>frames)available=frames;
    size_t consumed=(size_t)available;
    for(size_t frame=0;frame<consumed;++frame){
        size_t slot=(size_t)((tail+frame)%NS6_CAPTURE_RING_CAPACITY)*NS6_CAPTURE_RING_CHANNELS;
        output[frame*NS6_CAPTURE_RING_CHANNELS]=ring->samples[slot];
        output[frame*NS6_CAPTURE_RING_CHANNELS+1]=ring->samples[slot+1];
    }
    for(size_t frame=consumed;frame<frames;++frame){
        output[frame*NS6_CAPTURE_RING_CHANNELS]=0.0f;
        output[frame*NS6_CAPTURE_RING_CHANNELS+1]=0.0f;
    }
    atomic_store_explicit(&ring->tail,tail+consumed,memory_order_release);
    return consumed;
}

#endif
