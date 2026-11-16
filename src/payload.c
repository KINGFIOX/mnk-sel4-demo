#include <stdint.h>
#include <stddef.h>
#include "mnk.h"

extern char __image_start[];
extern char __image_end[];
extern char POOL_VA[];
extern char MARK_VA[];
extern char TRACE_VA[];
extern char CSTACK_VA[];
extern char GSTACK_VA[];

extern void ctx_switch(void *save, const void *load);
extern uint64_t sched_call(uint64_t cap, uint64_t msginfo, uint64_t mr0);

void worker_entry(void);
void worker_landing(void);
void mnk_yield(void);
int mnk_mutex_trylock(volatile uint64_t *lock);
void mnk_mutex_lock(volatile uint64_t *lock);
void mnk_mutex_unlock(volatile uint64_t *lock);
struct shared_state *mnk_shared(void);
void *mnk_stats(void);

const struct image_manifest __manifest __attribute__((section(".manifest"), used)) = {
    .magic = MANIFEST_MAGIC,
    .image_base = (uint64_t)(uintptr_t)__image_start,
    .image_end = (uint64_t)(uintptr_t)__image_end,
    .worker_entry_va = (uint64_t)(uintptr_t)worker_entry,
    .worker_landing_va = (uint64_t)(uintptr_t)worker_landing,
    .pool_va = (uint64_t)(uintptr_t)POOL_VA,
    .mark_va = (uint64_t)(uintptr_t)MARK_VA,
    .trace_va = (uint64_t)(uintptr_t)TRACE_VA,
    .cstack_va = (uint64_t)(uintptr_t)CSTACK_VA,
    .gstack_va = (uint64_t)(uintptr_t)GSTACK_VA,
    .api = {
        [API_YIELD] = (uint64_t)(uintptr_t)mnk_yield,
        [API_MUTEX_LOCK] = (uint64_t)(uintptr_t)mnk_mutex_lock,
        [API_MUTEX_UNLOCK] = (uint64_t)(uintptr_t)mnk_mutex_unlock,
        [API_MUTEX_TRYLOCK] = (uint64_t)(uintptr_t)mnk_mutex_trylock,
        [API_SHARED] = (uint64_t)(uintptr_t)mnk_shared,
        [API_STATS] = (uint64_t)(uintptr_t)mnk_stats,
    },
};

static struct shared_page *pool(void)
{
    return (struct shared_page *)(void *)POOL_VA;
}

static uint64_t whoami(void)
{
    uint64_t tp;
    asm volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return (tp - IPCB_VA(0)) >> 12;
}

static uint64_t current_vspace(void)
{
    return *(volatile uint64_t *)MARK_VA;
}

static uint32_t trace_claim_slot(volatile uint32_t *cursor)
{
    return __atomic_fetch_add(cursor, 1, __ATOMIC_ACQ_REL);
}

void mnk_yield(void)
{
    struct shared_page *s = pool();
    uint64_t w = whoami();
    uint64_t g = s->workers[w].current_gid;
    volatile uint32_t *tr = (volatile uint32_t *)TRACE_VA;
    uint32_t slot = trace_claim_slot(&tr[TRACE_SLOTS]);
    tr[slot] = (uint32_t)s->threads[g].home_vspace << 30
             | (uint32_t)current_vspace() << 28
             | (uint32_t)w << 26
             | (uint32_t)g << 23
             | slot;
    s->threads[g].yields++;
    ctx_switch(&s->threads[g].ctx, &s->workers[w].scheduler_ctx);
}

int mnk_mutex_trylock(volatile uint64_t *lock)
{
    uint64_t expected = 0;
    return __atomic_compare_exchange_n(lock, &expected, 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void mnk_mutex_lock(volatile uint64_t *lock)
{
    while (!mnk_mutex_trylock(lock)) {
        mnk_yield();
    }
}

void mnk_mutex_unlock(volatile uint64_t *lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

struct shared_state *mnk_shared(void)
{
    return &pool()->shared;
}

void *mnk_stats(void)
{
    struct shared_page *s = pool();
    uint64_t w = whoami();
    return &s->threads[s->workers[w].current_gid].yields;
}

static __attribute__((noreturn)) void run_from(uint64_t me_id, uint64_t g_id)
{
    struct worker *me = &pool()->workers[me_id];
    for (;;) {
        me->current_gid = g_id;
        ctx_switch(&me->scheduler_ctx, &pool()->threads[g_id].ctx);
        g_id = sched_call(me->sched_ep, me->sched_msginfo, g_id);
    }
}

void worker_entry(void)
{
    uint64_t me_id = whoami();
    struct worker *me = &pool()->workers[me_id];
    run_from(me_id, sched_call(me->sched_ep, me->sched_msginfo, NG));
}

void worker_landing(void)
{
    uint64_t me_id = whoami();
    struct worker *me = &pool()->workers[me_id];
    asm volatile("dmb ish" ::: "memory");
    uint64_t g_id = me->pending_gid;
    me->pending_gid = NG;
    run_from(me_id, g_id);
}
