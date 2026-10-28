#include <assert.h>
#include <stdbool.h>
#include "NS6ConnectionPolicy.h"

int main(void) {
    assert(ns6_connection_should_defer_disconnect(false, 1));
    assert(ns6_connection_should_defer_disconnect(false, 3));
    assert(!ns6_connection_should_defer_disconnect(false, 0));
    assert(!ns6_connection_should_defer_disconnect(true, 1));
    return 0;
}
