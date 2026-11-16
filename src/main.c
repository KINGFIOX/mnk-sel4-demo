#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sel4/sel4.h>
#include <sel4/bootinfo_types.h>
#include <sel4runtime.h>
#include "mnk.h"

#define WORKER_PRIO     100

#define POOL_WINDOW     0x30000000UL
#define TRACE_A_WINDOW  0x30200000UL
#define TRACE_B_WINDOW  0x30400000UL
#define IMAGE_WINDOW    0x20080000UL

extern const unsigned char rt_a_start[];
extern const unsigned char rt_b_start[];
extern const unsigned char prog1_start[];
extern const unsigned char prog2_start[];
extern const unsigned char prog3_start[];
extern const unsigned char prog4_start[];
extern const unsigned char prog5_start[];

_Static_assert(sizeof(struct shared_page) <= 4096, "shared page overflow");
_Static_assert(offsetof(struct shared_page, threads) == POOL_OFF, "pool offset");
_Static_assert(sizeof(struct worker) == 224, "worker size");
_Static_assert(offsetof(struct worker, scheduler_ctx) == 120, "scheduler ctx offset");
_Static_assert(sizeof(struct user_thread) == SLOT_STRIDE, "user thread size");
_Static_assert(offsetof(struct user_thread, yields) == 112, "stats pair offset");
_Static_assert(offsetof(struct shared_page, shared) == POOL_OFF + NG * SLOT_STRIDE,
               "shared state offset");

struct elf64_ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct elf64_dyn {
    int64_t d_tag;
    uint64_t d_val;
};

struct elf64_sym {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};

struct elf64_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
};

struct space {
    const unsigned char *blob;
    seL4_CPtr vspace_root;
    seL4_CPtr mark_frame;
    seL4_CPtr trace_frame;
    struct image_manifest mf;
};

struct worker_state {
    const char *name;
    seL4_CPtr tcb;
    seL4_CPtr fault_ep;
    seL4_CPtr ipc_frame;
    seL4_CPtr cstack_frame;
    int start_space;
    int current_space;
    int done;
    int assignments;
    int assigns_in_space[2];
};

static seL4_BootInfo *bootinfo;
static seL4_CPtr next_free_slot = 0;
static struct space spaces[2];
static struct shared_page *sh;

static struct worker_state workers[3] = {
    {.name = "worker-a", .start_space = 0},
    {.name = "worker-b", .start_space = 1},
    {.name = "worker-c", .start_space = 0},
};
static seL4_Word program_entry[NG];

static const char *api_names[API_COUNT] = {
    "mnk_yield", "mnk_mutex_lock", "mnk_mutex_unlock",
    "mnk_mutex_trylock", "mnk_shared", "mnk_stats",
};

static seL4_CPtr alloc_slot(void)
{
    return bootinfo->empty.start + next_free_slot++;
}

static seL4_Error retype_obj(seL4_Uint64 type, int size_bits, seL4_CPtr dest)
{
    for (seL4_Word i = 0; i < bootinfo->untyped.end - bootinfo->untyped.start; i++) {
        seL4_UntypedDesc *d = &bootinfo->untypedList[i];
        if (d->isDevice || d->sizeBits < size_bits) {
            continue;
        }
        seL4_Error err = seL4_Untyped_Retype(bootinfo->untyped.start + i, type, size_bits,
                                             seL4_CapInitThreadCNode, 0, 0, dest, 1);
        if (err == seL4_NoError) {
            return 0;
        }
    }
    printf("!! retype(type=%llu bits=%d): no untyped fits\n",
           (unsigned long long)type, size_bits);
    return 1;
}

static seL4_Error map_page(seL4_CPtr frame, seL4_CPtr vspace, seL4_Word va)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        seL4_Error err = seL4_ARM_Page_Map(frame, vspace, va,
                                           seL4_CapRights_new(1, 1, 1, 1),
                                           seL4_ARM_Default_VMAttributes);
        if (err == seL4_NoError) {
            return 0;
        }
        seL4_CPtr pt = alloc_slot();
        if (retype_obj(seL4_ARM_PageTableObject, seL4_PageTableBits, pt)) {
            return 2;
        }
        err = seL4_ARM_PageTable_Map(pt, vspace, va, seL4_ARM_Default_VMAttributes);
        printf("    [pt ] add pagetable level @%#lx -> %d\n", va, err);
    }
    return 3;
}

static seL4_CPtr mint_badged(seL4_CPtr src, seL4_Word badge)
{
    seL4_CPtr dst = alloc_slot();
    seL4_Error e = seL4_CNode_Mint(seL4_CapInitThreadCNode, dst, seL4_WordBits,
                                   seL4_CapInitThreadCNode, src, seL4_WordBits,
                                   seL4_CapRights_new(1, 1, 1, 1), badge);
    if (e) printf("!! mint badge %d\n", e);
    return dst;
}

static seL4_CPtr dup_cap(seL4_CPtr src)
{
    return mint_badged(src, 0);
}

static seL4_CPtr new_page(void)
{
    seL4_CPtr frame = alloc_slot();
    if (retype_obj(seL4_ARM_SmallPageObject, seL4_PageBits, frame)) {
        printf("!! page alloc failed\n");
    }
    return frame;
}

static const struct elf64_phdr *prog_phdr(const struct elf64_ehdr *eh, int p)
{
    return (const struct elf64_phdr *)
        ((const uint8_t *)eh + eh->e_phoff + (uint32_t)p * eh->e_phentsize);
}

static const unsigned char *blob_vaddr(const struct elf64_ehdr *eh, uint64_t vaddr)
{
    for (int p = 0; p < eh->e_phnum; p++) {
        const struct elf64_phdr *ph = prog_phdr(eh, p);
        if (ph->p_type == 1 && vaddr >= ph->p_vaddr && vaddr < ph->p_vaddr + ph->p_filesz) {
            return (const unsigned char *)eh + ph->p_offset + (vaddr - ph->p_vaddr);
        }
    }
    return NULL;
}

static seL4_Error load_runtime(struct space *sp)
{
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)sp->blob;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E'
        || eh->e_phentsize != sizeof(struct elf64_phdr)) {
        printf("!! runtime blob is not a sane ELF64\n");
        return 1;
    }
    for (int p = 0; p < eh->e_phnum; p++) {
        const struct elf64_phdr *ph = prog_phdr(eh, p);
        if (ph->p_type != 1) {
            continue;
        }
        for (uint64_t off = 0; off < ph->p_memsz; ) {
            uint64_t in_page = (ph->p_vaddr + off) & 0xFFF;
            uint64_t page_rem = 0x1000 - in_page;
            seL4_Word va = (ph->p_vaddr + off) & ~0xFFFUL;
            seL4_CPtr frame = new_page();
            seL4_Error e = map_page(frame, seL4_CapInitThreadVSpace, IMAGE_WINDOW);
            if (e) {
                return e;
            }
            uint8_t *dst = (uint8_t *)IMAGE_WINDOW;
            memset(dst, 0, 0x1000);
            if (off < ph->p_filesz) {
                uint64_t chunk = ph->p_filesz - off;
                if (chunk > page_rem) {
                    chunk = page_rem;
                }
                memcpy(dst + in_page, sp->blob + ph->p_offset + off, chunk);
            }
            e = map_page(dup_cap(frame), sp->vspace_root, va);
            seL4_ARM_Page_Unmap(frame);
            if (e) {
                return e;
            }
            off += page_rem;
        }
    }
    const struct elf64_phdr *ph0 = prog_phdr(eh, 0);
    memcpy(&sp->mf, sp->blob + ph0->p_offset, sizeof(sp->mf));
    if (sp->mf.magic != MANIFEST_MAGIC) {
        printf("!! runtime manifest magic mismatch\n");
        return 1;
    }
    printf("    runtime %#lx..%#lx entry=%#lx landing=%#lx pool=%#lx mark=%#lx\n",
           (unsigned long)sp->mf.image_base, (unsigned long)sp->mf.image_end,
           (unsigned long)sp->mf.worker_entry_va, (unsigned long)sp->mf.worker_landing_va,
           (unsigned long)sp->mf.pool_va, (unsigned long)sp->mf.mark_va);
    return 0;
}

static uint64_t resolve_api_symbol(struct space *sp, const char *name)
{
    for (int i = 0; i < API_COUNT; i++) {
        if (strcmp(name, api_names[i]) == 0) {
            return sp->mf.api[i];
        }
    }
    return 0;
}

static seL4_Error apply_rela_range(struct space *sp, const struct elf64_ehdr *eh,
                                   seL4_Word base, const unsigned char *rela,
                                   uint64_t rela_bytes, seL4_Word page_va,
                                   const struct elf64_sym *symtab, const char *strtab)
{
    for (uint64_t o = 0; o + sizeof(struct elf64_rela) <= rela_bytes;
         o += sizeof(struct elf64_rela)) {
        const struct elf64_rela *r = (const struct elf64_rela *)(rela + o);
        seL4_Word target = base + (seL4_Word)r->r_offset;
        if ((target & ~0xFFFUL) != page_va) {
            continue;
        }
        uint32_t type = (uint32_t)(r->r_info & 0xffffffffu);
        uint32_t symidx = (uint32_t)(r->r_info >> 32);
        uint64_t value;
        if (type == 1027) {
            value = base + (uint64_t)r->r_addend;
        } else if (type == 1025 || type == 1026) {
            const struct elf64_sym *sym = &symtab[symidx];
            const char *name = strtab + sym->st_name;
            if (sym->st_shndx != 0) {
                value = base + sym->st_value;
            } else {
                value = resolve_api_symbol(sp, name);
                if (value == 0) {
                    printf("!! unresolved symbol in user program: %s\n", name);
                    return 1;
                }
            }
            value += (uint64_t)r->r_addend;
        } else {
            printf("!! unsupported relocation type %u in user program\n", type);
            return 1;
        }
        *(volatile uint64_t *)(IMAGE_WINDOW + (target & 0xFFF)) = value;
    }
    return 0;
}

static seL4_Error load_program(struct space *sp, const unsigned char *blob,
                               seL4_Word base, seL4_Word *entry_out)
{
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)blob;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E'
        || eh->e_phentsize != sizeof(struct elf64_phdr)) {
        printf("!! user program blob is not a sane ELF64\n");
        return 1;
    }

    uint64_t symtab_va = 0, strtab_va = 0;
    uint64_t rela_va = 0, rela_bytes = 0;
    uint64_t jmprel_va = 0, pltrela_bytes = 0;
    for (int p = 0; p < eh->e_phnum; p++) {
        const struct elf64_phdr *ph = prog_phdr(eh, p);
        if (ph->p_type != 2) {
            continue;
        }
        const struct elf64_dyn *d =
            (const struct elf64_dyn *)blob_vaddr(eh, ph->p_vaddr);
        for (; d->d_tag != 0; d++) {
            switch (d->d_tag) {
            case 5: strtab_va = d->d_val; break;
            case 6: symtab_va = d->d_val; break;
            case 7: rela_va = d->d_val; break;
            case 8: rela_bytes = d->d_val; break;
            case 2: pltrela_bytes = d->d_val; break;
            case 23: jmprel_va = d->d_val; break;
            default: break;
            }
        }
    }
    const struct elf64_sym *symtab = (const struct elf64_sym *)blob_vaddr(eh, symtab_va);
    const char *strtab = (const char *)blob_vaddr(eh, strtab_va);
    if (symtab == NULL || strtab == NULL) {
        printf("!! user program has no dynamic symbol/string table\n");
        return 1;
    }

    for (int p = 0; p < eh->e_phnum; p++) {
        const struct elf64_phdr *ph = prog_phdr(eh, p);
        if (ph->p_type != 1) {
            continue;
        }
        for (uint64_t off = 0; off < ph->p_memsz; ) {
            uint64_t in_page = (ph->p_vaddr + off) & 0xFFF;
            uint64_t page_rem = 0x1000 - in_page;
            seL4_Word va = (ph->p_vaddr + off + base) & ~0xFFFUL;
            seL4_CPtr frame = new_page();
            seL4_Error e = map_page(frame, seL4_CapInitThreadVSpace, IMAGE_WINDOW);
            if (e) {
                return e;
            }
            uint8_t *dst = (uint8_t *)IMAGE_WINDOW;
            memset(dst, 0, 0x1000);
            if (off < ph->p_filesz) {
                uint64_t chunk = ph->p_filesz - off;
                if (chunk > page_rem) {
                    chunk = page_rem;
                }
                memcpy(dst + in_page, blob + ph->p_offset + off, chunk);
            }
            if (rela_va && rela_bytes) {
                if (apply_rela_range(sp, eh, base, blob_vaddr(eh, rela_va),
                                     rela_bytes, va, symtab, strtab)) {
                    return 1;
                }
            }
            if (jmprel_va && pltrela_bytes) {
                if (apply_rela_range(sp, eh, base, blob_vaddr(eh, jmprel_va),
                                     pltrela_bytes, va, symtab, strtab)) {
                    return 1;
                }
            }
            e = map_page(dup_cap(frame), sp->vspace_root, va);
            seL4_ARM_Page_Unmap(frame);
            if (e) {
                return e;
            }
            off += page_rem;
        }
    }
    *entry_out = base + (seL4_Word)eh->e_entry;
    return 0;
}

static void print_banner(void)
{
#ifdef CONFIG_ENABLE_SMP_SUPPORT
    printf("\n=== M:N:K v8 userland: 5 programs > 3 workers + 1 scheduler > 2 vspace, %d vCPUs ===\n",
           CONFIG_MAX_NUM_NODES);
#else
    printf("\n=== M:N:K v8 userland: 5 programs > 3 workers + 1 scheduler > 2 vspace, 1 vCPU ===\n");
#endif
    printf("    programs are PIC modules: mnk_yield()/mnk_mutex_*() reached via GOT\n");
    printf("    slots the loader relocated; lock miss = yield\n");
}

static seL4_Error create_worker_objects(void)
{
    for (int w = 0; w < 3; w++) {
        struct worker_state *wk = &workers[w];
        wk->fault_ep = alloc_slot();
        wk->tcb = alloc_slot();
        wk->ipc_frame = alloc_slot();
        wk->cstack_frame = alloc_slot();
        if (retype_obj(seL4_EndpointObject, seL4_EndpointBits, wk->fault_ep) ||
            retype_obj(seL4_TCBObject, seL4_TCBBits, wk->tcb) ||
            retype_obj(seL4_ARM_SmallPageObject, seL4_PageBits, wk->ipc_frame) ||
            retype_obj(seL4_ARM_SmallPageObject, seL4_PageBits, wk->cstack_frame)) {
            printf("!! retype ep/tcb/frame failed\n");
            return 1;
        }
    }
    return 0;
}

static seL4_Error build_vspaces(void)
{
    spaces[0].blob = rt_a_start;
    spaces[1].blob = rt_b_start;
    for (int s = 0; s < 2; s++) {
        spaces[s].vspace_root = alloc_slot();
        if (retype_obj(seL4_ARM_VSpaceObject, seL4_VSpaceBits, spaces[s].vspace_root)) {
            return 1;
        }
        seL4_Error e = seL4_ARM_ASIDPool_Assign(seL4_CapInitThreadASIDPool,
                                                spaces[s].vspace_root);
        printf("[%d] VSpace %c root + ASID -> %d\n", s + 1, 'A' + s, e);
    }

    for (int s = 0; s < 2; s++) {
        printf("[%d] runtime image into VSpace %c:\n", s + 3, 'A' + s);
        if (load_runtime(&spaces[s])) {
            return 1;
        }
    }
    return 0;
}

static seL4_Error map_pool_frame(void)
{
    const seL4_CPtr pool_frame = new_page();
    seL4_Error e = map_page(pool_frame, seL4_CapInitThreadVSpace, POOL_WINDOW);
    e |= map_page(dup_cap(pool_frame), spaces[0].vspace_root, spaces[0].mf.pool_va);
    e |= map_page(dup_cap(pool_frame), spaces[1].vspace_root, spaces[1].mf.pool_va);
    printf("[5] one pool frame at three VAs: rootserver@%#lx A@%#lx B@%#lx -> %d\n",
           POOL_WINDOW, (unsigned long)spaces[0].mf.pool_va,
           (unsigned long)spaces[1].mf.pool_va, e);
    return e;
}

static seL4_Error map_mark_and_trace_frames(void)
{
    seL4_Error e = 0;
    for (int s = 0; s < 2; s++) {
        struct space *sp = &spaces[s];
        sp->mark_frame = new_page();
        sp->trace_frame = new_page();
        e |= map_page(sp->mark_frame, seL4_CapInitThreadVSpace, IMAGE_WINDOW);
        *(volatile uint64_t *)IMAGE_WINDOW = SPACE_A_ID + s;
        seL4_ARM_Page_Unmap(sp->mark_frame);
        e |= map_page(dup_cap(sp->mark_frame), sp->vspace_root, sp->mf.mark_va);
        e |= map_page(sp->trace_frame, sp->vspace_root, sp->mf.trace_va);
        e |= map_page(dup_cap(sp->trace_frame), seL4_CapInitThreadVSpace,
                      s == 0 ? TRACE_A_WINDOW : TRACE_B_WINDOW);
        memset((void *)(s == 0 ? TRACE_A_WINDOW : TRACE_B_WINDOW), 0, 0x1000);
    }
    printf("[6] mark frames (A=%d B=%d) + home-only trace frames -> %d\n",
           SPACE_A_ID, SPACE_B_ID, e);
    return e;
}

static seL4_Error map_worker_pages(void)
{
    seL4_Error e = 0;
    for (int w = 0; w < 3; w++) {
        e |= map_page(workers[w].ipc_frame, spaces[1].vspace_root, IPCB_VA(w));
        e |= map_page(dup_cap(workers[w].ipc_frame), spaces[0].vspace_root, IPCB_VA(w));
        e |= map_page(dup_cap(workers[w].cstack_frame), spaces[0].vspace_root,
                      spaces[0].mf.cstack_va + (seL4_Word)w * 0x1000UL);
        e |= map_page(dup_cap(workers[w].cstack_frame), spaces[1].vspace_root,
                      spaces[1].mf.cstack_va + (seL4_Word)w * 0x1000UL);
    }
    printf("[7] ipc frames same VA both spaces (kernel-fixed); worker stacks per-space VA -> %d\n", e);
    if (e) {
        return e;
    }

    for (int g = 0; g < NG; g++) {
        int s = (g < 3) ? 0 : 1;
        struct space *sp = &spaces[s];
        e |= map_page(new_page(), sp->vspace_root,
                      sp->mf.gstack_va + (seL4_Word)g * 0x1000UL);
    }
    printf("[8] G stacks mapped in home vspace only -> %d\n", e);
    return e;
}

static seL4_Error load_user_programs(void)
{
    const struct {
        const unsigned char *blob;
        seL4_Word base;
    } programs[NG] = {
        {prog1_start, 0x48000000UL},
        {prog2_start, 0x48123000UL},
        {prog3_start, 0x48456000UL},
        {prog4_start, 0x68000000UL},
        {prog5_start, 0x68123000UL},
    };
    printf("[9] loading PIC user programs at arbitrary bases, GOT -> runtime API:\n");
    for (int g = 0; g < NG; g++) {
        int s = (g < 3) ? 0 : 1;
        if (load_program(&spaces[s], programs[g].blob, programs[g].base, &program_entry[g])) {
            return 1;
        }
        printf("    program%d -> %c @%#lx entry=%#lx\n", g + 1, 'A' + s,
               (unsigned long)programs[g].base, (unsigned long)program_entry[g]);
    }
    return 0;
}

static void arm_pool(seL4_CPtr sched_ep)
{
    seL4_MessageInfo_t yield_info = seL4_MessageInfo_new(0, 0, 0, 1);
    sh = (struct shared_page *)POOL_WINDOW;
    memset(sh, 0, sizeof(*sh));
    for (int w = 0; w < 3; w++) {
        sh->workers[w].pending_gid = NG;
        sh->workers[w].current_gid = NG;
        sh->workers[w].sched_ep = mint_badged(sched_ep, (seL4_Word)w + 1);
        sh->workers[w].sched_msginfo = yield_info.words[0];
    }
    for (int g = 0; g < NG; g++) {
        int s = (g < 3) ? 0 : 1;
        struct space *sp = &spaces[s];
        sh->threads[g].home_vspace = SPACE_A_ID + s;
        sh->threads[g].ctx[CTX_X30] = program_entry[g];
        sh->threads[g].ctx[CTX_SP] = STACK_TOP(sp->mf.gstack_va + (seL4_Word)g * 0x1000UL);
    }
    printf("[10] pool armed: homes A A A B B; program1..5 armed with entry+stack\n");
}

static void arm_workers(void)
{
#ifdef CONFIG_ENABLE_SMP_SUPPORT
    const int worker_cpu[3] = {0, 1, 1};
#endif
    for (int w = 0; w < 3; w++) {
        struct worker_state *wk = &workers[w];
        struct space *sp = &spaces[wk->start_space];
        wk->current_space = wk->start_space;
        seL4_Error e = seL4_TCB_SetSpace(wk->tcb, wk->fault_ep, seL4_CapInitThreadCNode, 0,
                                          sp->vspace_root, 0);
        if (e) printf("!! SetSpace(%s) %d\n", wk->name, e);
        seL4_TCB_SetIPCBuffer(wk->tcb, IPCB_VA(w), wk->ipc_frame);
        seL4_TCB_SetPriority(wk->tcb, seL4_CapInitThreadTCB, WORKER_PRIO);
#ifdef CONFIG_ENABLE_SMP_SUPPORT
        seL4_TCB_SetAffinity(wk->tcb, worker_cpu[w]);
#endif
        seL4_UserContext regs;
        memset(&regs, 0, sizeof(regs));
        regs.pc = sp->mf.worker_entry_va;
        regs.sp = STACK_TOP(sp->mf.cstack_va + (seL4_Word)w * 0x1000UL);
        regs.tpidr_el0 = IPCB_VA(w);
        seL4_TCB_WriteRegisters(wk->tcb, false, 0,
                                sizeof(regs) / sizeof(seL4_Word), &regs);
        seL4_DebugNameThread(wk->tcb, wk->name);
    }
    printf("[11] workers armed: a->A@cpu0 b->B@cpu1 c->A@cpu1, fault eps -> rootserver\n");
}

static void launch_workers(void)
{
    for (int w = 0; w < 3; w++) {
        seL4_TCB_Resume(workers[w].tcb);
    }
    seL4_DebugDumpScheduler();
    seL4_TCB_SetPriority(seL4_CapInitThreadTCB, seL4_CapInitThreadTCB, 90);
    printf("[12] resumed 3 workers @%d; scheduler demoted to 90; budget %d yields\n",
           WORKER_PRIO, ASSIGN_BUDGET);
}

static void park_worker(struct worker_state *wk, const char *reason)
{
    seL4_TCB_Suspend(wk->tcb);
    printf("\n%s: %s\n", wk->name, reason);
    wk->done = 1;
}

static uint64_t run_scheduler(seL4_CPtr sched_ep)
{
    int g_running[NG] = {0};
    uint64_t rr_cursor = NG - 1;
    uint64_t assignments_left = ASSIGN_BUDGET;
    uint64_t redirects = 0;
    int finished = 0;
    while (finished < 3) {
        for (int w = 0; w < 3; w++) {
            struct worker_state *wk = &workers[w];
            if (wk->done) {
                continue;
            }
            seL4_Word fb = 0;
            seL4_MessageInfo_t fm = seL4_NBRecv(wk->fault_ep, &fb);
            if (seL4_MessageInfo_get_label(fm) != seL4_Fault_NullFault) {
                printf("\n!! %s stray VMFault pc=%#lx addr=%#lx (enforcement hit), parking\n",
                       wk->name, seL4_GetMR(0), seL4_GetMR(1));
                seL4_TCB_Suspend(wk->tcb);
                wk->done = 1;
                finished++;
            }
        }

        seL4_Word badge = 0;
        seL4_Recv(sched_ep, &badge);
        if (badge == 0 || badge > 3) {
            continue;
        }
        int w = (int)badge - 1;
        struct worker_state *wk = &workers[w];
        if (wk->done) {
            continue;
        }
        uint64_t yielded = seL4_GetMR(0);
        if (yielded < NG) {
            g_running[yielded] = 0;
        }
        if (assignments_left == 0) {
            park_worker(wk, "budget exhausted, suspended by scheduler");
            finished++;
            continue;
        }
        int next = -1;
        for (uint64_t step = 1; step <= NG; step++) {
            uint64_t id = (rr_cursor + step) % NG;
            if (!g_running[id]) {
                next = (int)id;
                break;
            }
        }
        if (next < 0) {
            park_worker(wk, "every program already running elsewhere, suspended");
            finished++;
            continue;
        }
        rr_cursor = (uint64_t)next;
        g_running[next] = 1;
        assignments_left--;
        wk->assignments++;
        int home = (next < 3) ? 0 : 1;
        wk->assigns_in_space[home]++;

        if (home == wk->current_space) {
            seL4_SetMR(0, (seL4_Word)next);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else {
            sh->workers[w].pending_gid = (uint64_t)next;
            seL4_TCB_Suspend(wk->tcb);
            seL4_TCB_SetSpace(wk->tcb, wk->fault_ep, seL4_CapInitThreadCNode, 0,
                              spaces[home].vspace_root, 0);
            seL4_UserContext regs;
            memset(&regs, 0, sizeof(regs));
            regs.pc = spaces[home].mf.worker_landing_va;
            regs.sp = STACK_TOP(spaces[home].mf.cstack_va + (seL4_Word)w * 0x1000UL);
            seL4_TCB_WriteRegisters(wk->tcb, true, 0, 2, &regs);
            wk->current_space = home;
            redirects++;
        }
    }
    return redirects;
}

static void report_scheduler(uint64_t redirects)
{
    printf("\n=== scheduler bookkeeping ===\n");
    printf("  assignments: a=%d b=%d c=%d (total %d)\n",
           workers[0].assignments, workers[1].assignments, workers[2].assignments,
           workers[0].assignments + workers[1].assignments + workers[2].assignments);
    for (int w = 0; w < 3; w++) {
        printf("    worker-%c assigned %d A-home Gs, %d B-home Gs\n",
               'a' + w, workers[w].assigns_in_space[0], workers[w].assigns_in_space[1]);
    }
    printf("  cross-space redirects (Suspend+SetSpace+WriteRegisters): %lu\n",
           (unsigned long)redirects);
}

static void report_invariants(void)
{
    printf("\n=== program stats & example invariants ===\n");
    uint64_t lock_sum = sh->threads[0].locks_taken + sh->threads[1].locks_taken;
    printf("  program1 (A, lock):    yields=%lu locks=%lu\n",
           (unsigned long)sh->threads[0].yields, (unsigned long)sh->threads[0].locks_taken);
    printf("  program2 (A, lock):    yields=%lu locks=%lu\n",
           (unsigned long)sh->threads[1].yields, (unsigned long)sh->threads[1].locks_taken);
    printf("  lock example: p1+p2 locks=%lu, global_counter=%lu -> %s\n",
           (unsigned long)lock_sum, (unsigned long)sh->shared.global_counter,
           lock_sum == sh->shared.global_counter ? "atomicity holds" : "MUTEX VIOLATION");

    uint64_t ring_sum = 0;
    for (uint64_t i = sh->shared.ring_head; i != sh->shared.ring_tail;
         i = (i + 1) % 8) {
        ring_sum += sh->shared.ring[i];
    }
    uint64_t produced = sh->shared.produced;
    uint64_t expected = produced * (produced + 1) / 2;
    printf("  program3 (A, fib):     yields=%lu locks=%lu pair=(%lu,%lu)\n",
           (unsigned long)sh->threads[2].yields, (unsigned long)sh->threads[2].locks_taken,
           (unsigned long)sh->threads[2].result_a, (unsigned long)sh->threads[2].result_b);
    uint64_t fa = sh->threads[2].result_a;
    uint64_t fb = sh->threads[2].result_b;
    uint64_t fib_steps = 0;
    bool fib_valid = false;
    while (fib_steps < 200) {
        if (fa == 1 && (fb == 1 || fb == 2)) {
            fib_valid = true;
            break;
        }
        if (fb <= fa || fb - fa > fa) {
            break;
        }
        uint64_t prev = fb - fa;
        fb = fa;
        fa = prev;
        fib_steps++;
    }
    printf("     fib pair walk-back: %s (%lu steps to seed 1,1)\n",
           fib_valid ? "genuine consecutive Fibonacci pair" : "CORRUPTED",
           (unsigned long)fib_steps);
    printf("  program4 (B, produce): yields=%lu produced=%lu\n",
           (unsigned long)sh->threads[3].yields, (unsigned long)produced);
    printf("  program5 (B, consume): yields=%lu consumed_sum=%lu ring_left=%lu\n",
           (unsigned long)sh->threads[4].yields,
           (unsigned long)sh->shared.consumed_sum,
           (unsigned long)((sh->shared.ring_tail + 8 - sh->shared.ring_head) % 8));
    printf("  ring example: consumed_sum+ring_sum=%lu, produced*(produced+1)/2=%lu -> %s\n",
           (unsigned long)(sh->shared.consumed_sum + ring_sum), (unsigned long)expected,
           sh->shared.consumed_sum + ring_sum == expected
               ? "no loss, no duplication" : "RING VIOLATION");
}

static void report_trace(void)
{
    volatile uint32_t *trace[2] = {(volatile uint32_t *)TRACE_A_WINDOW,
                                   (volatile uint32_t *)TRACE_B_WINDOW};
    uint32_t count[2];
    for (int f = 0; f < 2; f++) {
        count[f] = trace[f][TRACE_SLOTS];
        if (count[f] > TRACE_SLOTS) {
            count[f] = TRACE_SLOTS;
        }
    }
    printf("\n=== trace decode: %u yields in A + %u in B ===\n", count[0], count[1]);

    int events_per_thread[NG] = {0};
    int events_per_worker[3] = {0};
    int observed_per_worker[3][2] = {{0}};
    int workers_per_thread[NG] = {0};
    int home_observed_mismatches = 0;
    int total = 0;
    for (int f = 0; f < 2; f++) {
        for (uint32_t i = 0; i < count[f]; i++) {
            uint32_t w = trace[f][i];
            int home = w >> 30;
            int observed = (w >> 28) & 3;
            int worker = (w >> 26) & 3;
            int thread = (w >> 23) & 7;
            if (home < SPACE_A_ID || home > SPACE_B_ID || worker > 2 || thread >= NG) {
                continue;
            }
            events_per_thread[thread]++;
            events_per_worker[worker]++;
            observed_per_worker[worker][home - SPACE_A_ID]++;
            workers_per_thread[thread] |= 1 << worker;
            if (home != observed) {
                home_observed_mismatches++;
            }
            total++;
            if (f == 0 && i < 10) {
                printf("  A#%03u: program%d home=%c on worker-%c observed=%c\n", i,
                       thread + 1, 'A' + home - 1, 'a' + worker, 'A' + observed - 1);
            }
        }
    }
    printf("\n  yields total=%d per program: p1=%d p2=%d p3=%d p4=%d p5=%d\n",
           total, events_per_thread[0], events_per_thread[1], events_per_thread[2],
           events_per_thread[3], events_per_thread[4]);
    printf("  per worker: a=%d b=%d c=%d\n", events_per_worker[0],
           events_per_worker[1], events_per_worker[2]);
    for (int w = 0; w < 3; w++) {
        printf("    worker-%c: %d yields in A, %d in B\n",
               'a' + w, observed_per_worker[w][0], observed_per_worker[w][1]);
    }
    printf("  home==observed violations: %d\n", home_observed_mismatches);
    for (int g = 0; g < NG; g++) {
        printf("    program%d ran on:", g + 1);
        for (int w = 0; w < 3; w++) {
            if (workers_per_thread[g] & (1 << w)) {
                printf(" worker-%c", 'a' + w);
            }
        }
        printf("\n");
    }
}

static void report_summary(void)
{
    printf("\n=== summary ===\n");
    printf("  M=5 user programs: PIC modules, infinite loops calling mnk_yield();\n");
    printf("     the only absolute addresses are GOT slots the loader filled with\n");
    printf("     runtime API addresses (yield + mutex + shared + stats)\n");
    printf("  N=4 kernel threads: 3 workers + 1 scheduler; yield = Call on badged ep;\n");
    printf("     cross-space = Suspend+SetSpace+WriteRegisters; mutex miss = yield\n");
    printf("  K=2 program vspaces: A@%#lx (p1..p3), B@%#lx (p4..p5), all VAs differ\n",
           (unsigned long)spaces[0].mf.image_base, (unsigned long)spaces[1].mf.image_base);
}

int main(void)
{
    bootinfo = sel4runtime_bootinfo();
    print_banner();

    const seL4_CPtr sched_ep = alloc_slot();
    if (retype_obj(seL4_EndpointObject, seL4_EndpointBits, sched_ep)
        || create_worker_objects() || build_vspaces() || map_pool_frame()
        || map_mark_and_trace_frames() || map_worker_pages() || load_user_programs()) {
        return 1;
    }

    arm_pool(sched_ep);

    arm_workers();
    launch_workers();

    uint64_t redirects = run_scheduler(sched_ep);

    report_scheduler(redirects);
    report_invariants();
    report_trace();
    report_summary();

    for (;;) {
        seL4_TCB_Suspend(seL4_CapInitThreadTCB);
    }
    return 0;
}
