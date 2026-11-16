#include "mnk_user.h"

void program_main(void)
{
    struct mnk_shared_state *shared = mnk_shared();
    struct mnk_thread_stats *mine = mnk_stats();
    uint64_t next_item = 1;

    for (;;) {
        mnk_mutex_lock(&shared->global_lock);
        if ((shared->ring_tail + 1) % MNK_RING_SLOTS != shared->ring_head) {
            shared->ring[shared->ring_tail] = next_item;
            shared->ring_tail = (shared->ring_tail + 1) % MNK_RING_SLOTS;
            shared->produced += 1;
            next_item += 1;
            mine->locks_taken += 1;
        }
        mnk_mutex_unlock(&shared->global_lock);

        for (volatile uint64_t spin = 0; spin < 120000; spin++) {
        }
        mnk_yield();
    }
}
