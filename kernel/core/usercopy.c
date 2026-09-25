/* SPDX-License-Identifier: Apache-2.0 */
/*
 * usercopy.c — the kernel's accesses to user memory, and there is one
 * direction left.
 *
 * `user_range_readable`, `copy_from_user_checked` and `copy_user_cstr_bounded`
 * are GONE, and what removed them was ledger A-33.  A message used to be a
 * struct in user memory named by a pointer: the kernel validated the range and
 * copied it in, which is what a READ path was for.  A message is registers
 * now, and a bulk payload lives in a frame the thread REGISTERED — which the
 * kernel reaches through its own physical window, not through a user pointer.
 * So the last caller went, and the functions sat here with none.
 *
 * What is left is write-back, and it is the whole list: `Boot_KlogDrain`,
 * `Boot_SchedInfo`, `Boot_FramebufferInfo`, `Untyped_Info`, `Untyped_Query`,
 * `TCB_GetInfo`, `TCB_ReadRegs` and `Notification_Poll`.  Every one of them
 * answers a question INTO a buffer the caller named, and none of them takes
 * anything but the address from it.
 *
 * That is worth stating as a property rather than as an absence: there is no
 * TOCTOU window on any input the kernel acts on, because it does not read its
 * inputs from memory a second thread can unmap.  seL4's arrangement is the
 * same one — it reads the IPC buffer through its own mapping and nothing else.
 */
#include <iris/usercopy.h>
#include <iris/paging.h>
#include <stdint.h>

#define USER_ADDR_MIN 0x1000ULL

/* When SMAP is active (CR4.SMAP=1), any supervisor-mode load or store to a
 * user page without STAC causes a #PF.  STAC sets RFLAGS.AC to temporarily
 * permit the access; CLAC clears it afterward.  When SMAP is not active
 * (older CPU or not enabled at boot), these inlines are no-ops. */
static inline void user_access_begin(void) {
    if (iris_smap_enabled) __asm__ volatile ("stac" ::: "memory");
}
static inline void user_access_end(void) {
    if (iris_smap_enabled) __asm__ volatile ("clac" ::: "memory");
}

static int user_range_accessible(uint64_t ptr, uint32_t len, uint64_t required_flags) {
    uint64_t end;
    uint64_t page;

    if (ptr == 0) return 0;
    if (ptr < USER_ADDR_MIN) return 0;
    if (len == 0) return 0;

    end = ptr + (uint64_t)len;
    if (end < ptr) return 0;
    if (end > USER_SPACE_TOP) return 0;

    page = ptr & ~0xFFFULL;
    end  = (end - 1ULL) & ~0xFFFULL;
    for (; page <= end; page += 0x1000ULL) {
        uint64_t flags = 0;
        if (paging_query_access(page, &flags) != 0) return 0;
        if ((flags & PAGE_PRESENT) == 0) return 0;
        if ((flags & PAGE_USER) == 0) return 0;
        if ((flags & required_flags) != required_flags) return 0;
    }
    return 1;
}

int user_range_writable(uint64_t ptr, uint32_t len) {
    return user_range_accessible(ptr, len, PAGE_WRITABLE);
}

/*
 * ── The exception table ────────────────────────────────────────────────────
 *
 * Validating a range and then writing it is two steps, and the mapping can go
 * away between them.  Nothing serialises this against a concurrent
 * `FRAME_UNMAP` on the same address space -- and a spawner holds VSpace
 * capabilities for its children -- so on SMP the store can land after the PTE
 * is retired and its TLB entry shot down.
 *
 * Before this table existed, that store faulted at CPL 0, and `idt.c` halts
 * the machine on any fault that did not come from ring 3.  Ring 3 could stop
 * the kernel.  Ledger A-37 recorded it as a divergence from seL4, which proves
 * its kernel never faults; this is the mechanism that turns the promise into a
 * property IRIS can actually hold.
 *
 * The check is NOT removed and the table is not a substitute for it.  The
 * check is what refuses a bad address; the table is what survives a good
 * address that stopped being one.  A fault here means the range was valid when
 * it was looked at, so the answer is "this call failed", not "this caller was
 * malicious".
 */
extern const uint64_t __ex_table_start[];
extern const uint64_t __ex_table_end[];

/*
 * How many faults the table has absorbed.
 *
 * It is a COUNTER rather than a flag because the number is the only way to see
 * this mechanism work at all: the path it protects is a race that a test
 * cannot schedule, so what a test can do is fire the instruction at an address
 * that is certain to fault and watch this go up.  A mechanism with no
 * observable effect is a mechanism nobody can tell is broken.
 */
static _Atomic uint32_t exfixup_taken;

void exfixup_stat_taken(void) {
    __atomic_fetch_add(&exfixup_taken, 1u, __ATOMIC_RELAXED);
}

uint32_t exfixup_taken_count(void) {
    return __atomic_load_n(&exfixup_taken, __ATOMIC_RELAXED);
}

uint64_t exfixup_lookup(uint64_t fault_rip) {
    const uint64_t *e = __ex_table_start;
    /* A linear scan over a table with one entry in it.  Sorting and bisecting
     * would be arranging for a size this table does not have. */
    for (; e + 1 < __ex_table_end; e += 2)
        if (e[0] == fault_rip) return e[1];
    return 0;
}

/* Implemented in usercopy_asm.S: one `rep movsb`, one exception-table entry,
 * and %rcx left holding what it did not manage to write. */
extern unsigned long usercopy_store(void *dst, const void *src, unsigned long len);

int copy_to_user_checked(uint64_t dst_uptr, const void *src, uint32_t len) {
    if (!src || !user_range_writable(dst_uptr, len)) return 0;
    user_access_begin();
    unsigned long left = usercopy_store((void *)(uintptr_t)dst_uptr, src, len);
    user_access_end();
    /* A partial write is still a failure, and the caller is told so rather
     * than being handed a half-filled buffer it has no way to measure. */
    return left == 0ul;
}
