#include "mnk_user.h"

void program_main(void)
{
    struct mnk_thread_stats *mine = mnk_stats();
    uint64_t fib_prev = 1;
    uint64_t fib_curr = 1;

    for (;;) {
        uint64_t fib_next = fib_prev + fib_curr;
        fib_prev = fib_curr;
        fib_curr = fib_next;
        if (fib_curr > 4000000000000000000ULL) {
            fib_prev = 1;
            fib_curr = 1;
        }
        mine->result_a = fib_prev;
        mine->result_b = fib_curr;

        for (volatile uint64_t spin = 0; spin < 90000; spin++) {
        }
        mnk_yield();
    }
}
