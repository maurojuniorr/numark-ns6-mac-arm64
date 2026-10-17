#ifndef NS6_IO_CLIENT_ACCOUNTING_H
#define NS6_IO_CLIENT_ACCOUNTING_H

#include <stdbool.h>

static inline bool ns6_io_client_needs_service_activation(unsigned active_starts){
    return active_starts==0;
}

static inline void ns6_io_client_account_start(unsigned *client_starts,
    unsigned *active_starts){
    ++*client_starts;
    ++*active_starts;
}

static inline void ns6_io_client_account_untracked_start(unsigned *untracked_starts,
    unsigned *active_starts){
    if(!untracked_starts||!active_starts)return;
    ++*untracked_starts;
    ++*active_starts;
}

static inline unsigned ns6_io_client_account_remove(unsigned *client_starts,
    unsigned *active_starts){
    if(!client_starts||!active_starts)return 0;
    unsigned removed=*client_starts;
    *client_starts=0;
    *active_starts=*active_starts>=removed?*active_starts-removed:0;
    return removed;
}

static inline bool ns6_io_client_account_stop(unsigned *client_starts,
    unsigned *untracked_starts,unsigned *active_starts){
    if(!active_starts||!*active_starts)return false;
    if(client_starts&&*client_starts)--*client_starts;
    else if(untracked_starts&&*untracked_starts)--*untracked_starts;
    else return false;
    --*active_starts;
    return true;
}

#endif
