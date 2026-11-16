#include "mnk_user.h"

void program_main(void)
{
    struct mnk_shared_state *shared = mnk_shared();
    struct mnk_thread_stats *mine = mnk_stats();

    for (;;) {
        mnk_mutex_lock(&shared->global_lock);
        shared->global_counter += 1;
        mine->locks_taken += 1;
        mnk_mutex_unlock(&shared->global_lock);

        for (volatile uint64_t spin = 0; spin < 65000; spin++) {
        }
        mnk_yield();
    }
}
