#ifndef NS6_IO_CLIENT_ACCOUNTING_H
#define NS6_IO_CLIENT_ACCOUNTING_H

#include <stdbool.h>

static inline void ns6_io_client_account_start(unsigned *client_starts,
    unsigned *active_starts){
    ++*client_starts;
    ++*active_starts;
}

static inline bool ns6_io_client_account_stop(unsigned *client_starts,
    unsigned *active_starts){
    if(!client_starts||!*client_starts||!active_starts||!*active_starts)
        return false;
    --*client_starts;
    --*active_starts;
    return true;
}

#endif
