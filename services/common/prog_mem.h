/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_PROG_MEM_H
#define IRIS_PROG_MEM_H

#include <stdint.h>

/*
 * Memory a program asks for, and the only thing that bounds it.
 *
 * `brk` and `mmap` here are not syscalls and are not a service.  A program
 * holds its own budget (`IRIS_CPTR_OWN_UNTYPED`) and its own address space
 * (`IRIS_CPTR_OWN_VSPACE`), so growing a heap is retyping a frame out of that
 * budget and mapping it into that address space — two invocations the program
 * is already authorised to make, on objects nobody else has to be asked about.
 *
 * That is the whole seL4 answer to "where does memory come from", and it has a
 * property the POSIX one does not: what a program can allocate is exactly what
 * it was GIVEN, visibly, at launch.  There is no global pool to exhaust, no
 * OOM killer to arbitrate, and no accounting the kernel has to keep — when the
 * budget is spent the retype refuses, and the refusal reaches the caller as a
 * return value rather than as a signal.
 *
 * ── What this is NOT ───────────────────────────────────────────────────────
 *
 * It is not a virtual-address allocator.  It is a CURSOR: `mmap` hands out the
 * next address in the region `docs/contracts/program.md` §3 fixes and never
 * reuses one.  `prog_munmap` therefore returns the MEMORY and not the address,
 * which is honest about what it does — a program that maps and unmaps in a
 * loop runs out of region long before it runs out of budget, and that is a
 * thing to fix when something needs it rather than a thing to pretend about.
 *
 * It is not a `malloc`.  A `malloc` sits ON this; that is step 6's business.
 *
 * ── Why it is two files ────────────────────────────────────────────────────
 *
 * `prog_mem_plan.c` is arithmetic — where the next mapping goes, which CSpace
 * slot is free, which record covers an address — and is compiled into the host
 * unit suite, where a wrong answer is a failed assertion instead of a page
 * fault in a virtual machine.  `prog_mem.c` is the invocations, and cannot be
 * anywhere but a program.
 */

/*
 * Protection, in the kernel's own spelling: `map_flags` bit 0 is writable and
 * bit 1 is executable.  No translation layer, deliberately — W^X is enforced
 * by the kernel (both bits together is `IRIS_ERR_INVALID_ARG`), and a
 * translation layer is where a system quietly acquires a third answer.
 */
#define PROG_PROT_R   0u
#define PROG_PROT_RW  1u
#define PROG_PROT_RX  2u

/* One mapping this program made.  Frames are kept because a frame capability
 * IS the mapping: delete it and the pages go, which is also how everything
 * here is reclaimed when the program dies and its CSpace is torn down. */
struct prog_map {
    uint64_t vaddr;
    uint64_t bytes;
    uint32_t slot;   /* the frame capability, in this program's own CSpace */
    uint32_t prot;
    uint32_t heap;   /* 1 if it backs the break, 0 if it came from mmap */
};

#define PROG_MEM_MAPS 32u

struct prog_mem {
    uint64_t untyped_c, vspace_c;
    uint32_t slot_next, slot_end, pt_slot;
    uint64_t brk_base, brk_cur, brk_end;   /* the break, and its ceiling   */
    uint64_t heap_top;                     /* how far frames actually reach */
    uint64_t mmap_cur, mmap_end;
    uint32_t nmaps;
    struct prog_map maps[PROG_MEM_MAPS];
};

/* ── the plan (prog_mem_plan.c): pure, and unit-tested on the host ───────── */

/*
 * Lay out the cursors from the contract's offsets.
 *
 * `private_base` is `USER_PRIVATE_BASE`, passed rather than included so this
 * half needs no kernel header and can be compiled for the host.  The CSpace
 * range is the caller's to choose: a program's own slots are 32..63 and
 * 128..255, and which of those a runtime spends on mappings is a runtime's
 * decision, not this file's.
 */
void prog_mem_setup(struct prog_mem *m, uint64_t private_base,
                    uint64_t untyped_c, uint64_t vspace_c,
                    uint32_t slot_lo, uint32_t slot_hi, uint32_t pt_slot);

/* Round up to whole pages, saturating rather than wrapping: a caller that asks
 * for a size within 4095 of 2^64 gets a refusal, not a tiny allocation. */
uint64_t prog_mem_pages(uint64_t bytes);

/*
 * Reserve address space.  `heap` picks the cursor; the reservation is only
 * refused when the REGION is out, which is a different failure from the budget
 * being out and is reported separately for that reason.  Returns 1 and writes
 * `*out_va`, or 0.
 */
int prog_mem_reserve(struct prog_mem *m, uint64_t bytes, int heap,
                     uint64_t *out_va);

/*
 * How much the heap grows when it must grow at all, given the bytes needed.
 *
 * Geometric, and the reason is the record table rather than speed: every frame
 * costs one entry in `maps` and one CSpace slot, both fixed, so a heap that
 * grew by the requested amount would run out of BOOKKEEPING long before it ran
 * out of budget — and then the refusal a program got would be about this file
 * instead of about what it was given.  Doubling makes the budget the thing
 * that runs out, which is the only bound this design wants to have.
 *
 * Floored at 64 KiB so the first few allocations do not each cost a frame.
 */
uint64_t prog_mem_chunk(const struct prog_mem *m, uint64_t need);

/* Take the next CSpace slot, or refuse.  Slots are never returned to the pool:
 * see the note on `prog_munmap`. */
int prog_mem_take_slot(struct prog_mem *m, uint32_t *out_slot);

/* The record whose range CONTAINS [vaddr, vaddr+bytes), or -1.  `mprotect`
 * uses it, and refuses anything that is not covered by exactly one mapping —
 * splitting a frame is not something a frame can do. */
int prog_mem_find(const struct prog_mem *m, uint64_t vaddr, uint64_t bytes);

/* Remember / forget a mapping.  `record` refuses when the table is full, which
 * is a real limit and is reported rather than overwritten. */
int  prog_mem_record(struct prog_mem *m, uint64_t vaddr, uint64_t bytes,
                     uint32_t slot, uint32_t prot, int heap);
void prog_mem_forget(struct prog_mem *m, int index);

/* ── the doing (prog_mem.c): invocations, program-side only ──────────────── */

/*
 * Move the break.  Returns the OLD break, or `(uint64_t)-1`.
 *
 * Growing maps whole frames ahead of the break and never maps the same page
 * twice; shrinking moves the break and releases only the frames that end up
 * entirely above it, which is what a break is allowed to do and what every
 * real one does.
 */
uint64_t prog_brk(struct prog_mem *m, int64_t delta);

/* A fresh mapping at the next address in the `mmap` region, or 0. */
uint64_t prog_mmap(struct prog_mem *m, uint64_t bytes, uint32_t prot);

/* Return the memory (not the address).  0 on success. */
int prog_munmap(struct prog_mem *m, uint64_t vaddr);

/*
 * Change protection, as unmap-and-remap, because that is what the object model
 * has: a mapping's flags are an argument to `Frame_Map` and there is no
 * invocation that edits a PTE in place.  Refuses a range that is not exactly
 * inside one mapping.  0 on success.
 */
int prog_mprotect(struct prog_mem *m, uint64_t vaddr, uint64_t bytes,
                  uint32_t prot);

#endif /* IRIS_PROG_MEM_H */
