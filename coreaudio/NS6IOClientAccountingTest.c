#include "NS6IOClientAccounting.h"

#include <assert.h>
#include <stddef.h>

int main(void){
    assert(ns6_io_client_needs_service_activation(0));
    assert(!ns6_io_client_needs_service_activation(1));
    assert(!ns6_io_client_needs_service_activation(5));
    unsigned client_a=0,client_b=0,active=0;
    ns6_io_client_account_start(&client_a,&active);
    assert(active==1);
    assert(ns6_io_client_account_stop(&client_a,NULL,&active));
    assert(active==0);
    ns6_io_client_account_start(&client_b,&active);
    assert(active==1);
    assert(!ns6_io_client_account_stop(&client_a,NULL,&active));
    assert(active==1);
    assert(ns6_io_client_account_stop(&client_b,NULL,&active));
    assert(active==0);

    unsigned tracked_client=0,untracked=0;
    ns6_io_client_account_start(&tracked_client,&active);
    ns6_io_client_account_untracked_start(&untracked,&active);
    assert(active==2&&untracked==1);
    assert(ns6_io_client_account_stop(&tracked_client,&untracked,&active));
    assert(active==1);
    assert(ns6_io_client_account_stop(NULL,&untracked,&active));
    assert(active==0);
    assert(untracked==0);

    ns6_io_client_account_start(&tracked_client,&active);
    assert(ns6_io_client_account_remove(&tracked_client,&active)==1);
    assert(tracked_client==0&&untracked==0&&active==0);
    assert(!ns6_io_client_account_stop(NULL,&untracked,&active));
    assert(active==0&&untracked==0);
    assert(!ns6_io_client_account_stop(NULL,&untracked,&active));

    unsigned client_starts[]={1,0};
    untracked=0;
    active=1;
    assert(!ns6_io_client_account_stop(&client_starts[1],&untracked,&active));
    assert(active==1&&client_starts[0]==1&&client_starts[1]==0);
    assert(ns6_io_client_account_stop(&client_starts[0],&untracked,&active));
    assert(active==0&&client_starts[0]==0&&client_starts[1]==0);
    return 0;
}
