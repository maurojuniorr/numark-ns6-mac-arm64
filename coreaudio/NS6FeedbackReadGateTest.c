#include "NS6FeedbackReadGate.h"

#include <assert.h>

int main(void) {
    assert(!ns6_feedback_read_should_submit(false, false));
    assert(!ns6_feedback_read_should_submit(true, true));
    assert(ns6_feedback_read_should_submit(true, false));
    return 0;
}
