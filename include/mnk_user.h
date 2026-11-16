#pragma once
#include <stdint.h>

#define MNK_RING_SLOTS 8

struct mnk_shared_state {
    volatile uint64_t global_lock;
    volatile uint64_t global_counter;
    volatile uint64_t ring_head;
    volatile uint64_t ring_tail;
    volatile uint64_t produced;
    volatile uint64_t consumed_sum;
    volatile uint64_t ring[MNK_RING_SLOTS];
};

struct mnk_thread_stats {
    volatile uint64_t yields;
    volatile uint64_t locks_taken;
    volatile uint64_t result_a;
    volatile uint64_t result_b;
};

void mnk_yield(void);
int mnk_mutex_trylock(volatile uint64_t *lock);
void mnk_mutex_lock(volatile uint64_t *lock);
void mnk_mutex_unlock(volatile uint64_t *lock);
struct mnk_shared_state *mnk_shared(void);
struct mnk_thread_stats *mnk_stats(void);
