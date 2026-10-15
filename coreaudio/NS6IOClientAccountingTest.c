#include "NS6IOClientAccounting.h"

#include <assert.h>

int main(void){
    unsigned client_a=0,client_b=0,active=0;
    ns6_io_client_account_start(&client_a,&active);
    assert(active==1);
    assert(ns6_io_client_account_stop(&client_a,&active));
    assert(active==0);
    ns6_io_client_account_start(&client_b,&active);
    assert(active==1);
    assert(!ns6_io_client_account_stop(&client_a,&active));
    assert(active==1);
    assert(ns6_io_client_account_stop(&client_b,&active));
    assert(active==0);
    return 0;
}
