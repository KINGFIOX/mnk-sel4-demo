#include "mnk_user.h"

void program_main(void)
{
    struct mnk_shared_state *shared = mnk_shared();
    struct mnk_thread_stats *mine = mnk_stats();

    for (;;) {
        mnk_mutex_lock(&shared->global_lock);
        if (shared->ring_head != shared->ring_tail) {
            shared->consumed_sum += shared->ring[shared->ring_head];
            shared->ring_head = (shared->ring_head + 1) % MNK_RING_SLOTS;
            mine->locks_taken += 1;
        }
        mnk_mutex_unlock(&shared->global_lock);

        for (volatile uint64_t spin = 0; spin < 150000; spin++) {
        }
        mnk_yield();
    }
}
