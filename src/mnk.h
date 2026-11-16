#pragma once
#include <stdint.h>
#include <stddef.h>

#define NG             5
#define TRACE_SLOTS    500
#define ASSIGN_BUDGET  500
#define POOL_OFF       672
#define SLOT_STRIDE    144
#define SPIN_PER_ROUND 800000

#define SPACE_A_ID     1
#define SPACE_B_ID     2

#define IPCB_VA(c)     (0x70000000UL + (c) * 0x1000UL)
#define STACK_TOP(va)  ((va) + 0xFF0UL)

#define MANIFEST_MAGIC 0x4D4E4B3347505332UL

enum ctx_word {
    CTX_X19 = 0,
    CTX_X20,
    CTX_X21,
    CTX_X22,
    CTX_X23,
    CTX_X24,
    CTX_X25,
    CTX_X26,
    CTX_X27,
    CTX_X28,
    CTX_X29,
    CTX_X30,
    CTX_SP,
    CTX_WORDS,
};

enum mnk_api_id {
    API_YIELD = 0,
    API_MUTEX_LOCK,
    API_MUTEX_UNLOCK,
    API_MUTEX_TRYLOCK,
    API_SHARED,
    API_STATS,
    API_COUNT,
};

struct worker {
    volatile uint64_t pending_gid;
    volatile uint64_t current_gid;
    uint64_t sched_ep;
    uint64_t sched_msginfo;
    uint64_t reserved[11];
    uint64_t scheduler_ctx[CTX_WORDS];
};

struct user_thread {
    volatile uint64_t home_vspace;
    uint64_t ctx[CTX_WORDS];
    volatile uint64_t yields;
    volatile uint64_t locks_taken;
    volatile uint64_t result_a;
    volatile uint64_t result_b;
};

struct shared_state {
    volatile uint64_t global_lock;
    volatile uint64_t global_counter;
    volatile uint64_t ring_head;
    volatile uint64_t ring_tail;
    volatile uint64_t produced;
    volatile uint64_t consumed_sum;
    volatile uint64_t ring[8];
};

struct shared_page {
    struct worker workers[3];
    struct user_thread threads[NG];
    struct shared_state shared;
};

struct image_manifest {
    uint64_t magic;
    uint64_t image_base;
    uint64_t image_end;
    uint64_t worker_entry_va;
    uint64_t worker_landing_va;
    uint64_t pool_va;
    uint64_t mark_va;
    uint64_t trace_va;
    uint64_t cstack_va;
    uint64_t gstack_va;
    uint64_t api[API_COUNT];
};
