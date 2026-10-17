#include "NS6CaptureRing.h"
#include <assert.h>
#include <stddef.h>

int main(void){
    NS6CaptureRing ring;
    ns6_capture_ring_init(&ring);
    const float input[]={0.1f,-0.1f,0.2f,-0.2f,0.3f,-0.3f,0.4f,-0.4f};
    float output[10]={9.0f,9.0f,9.0f,9.0f,9.0f,9.0f,9.0f,9.0f,9.0f,9.0f};
    assert(ns6_capture_ring_write(&ring,input,4)==4);
    assert(ns6_capture_ring_read(&ring,output,3)==3);
    assert(output[0]==0.1f&&output[1]==-0.1f&&output[4]==0.3f&&output[5]==-0.3f);
    assert(ns6_capture_ring_read(&ring,output,3)==1);
    assert(output[0]==0.4f&&output[1]==-0.4f);
    assert(output[2]==0.0f&&output[3]==0.0f&&output[4]==0.0f&&output[5]==0.0f);
    assert(ns6_capture_ring_write(&ring,input,2)==2);
    ns6_capture_ring_clear(&ring);
    assert(ns6_capture_ring_read(&ring,output,2)==0);
    assert(output[0]==0.0f&&output[1]==0.0f&&output[2]==0.0f&&output[3]==0.0f);
    assert(ns6_capture_ring_write(&ring,NULL,0)==0);
    assert(ns6_capture_ring_read(&ring,NULL,0)==0);
    return 0;
}
