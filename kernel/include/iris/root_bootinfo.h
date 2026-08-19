#ifndef IRIS_ROOT_BOOTINFO_H
#define IRIS_ROOT_BOOTINFO_H

#include <stdint.h>
#ifdef __KERNEL__
#include <iris/nc/error.h>
#endif

/*
 * Stage 5 — the root task's BootInfo.
 *
 * NOT to be confused with <iris/boot_info.h>, which is the FIRMWARE→KERNEL
 * handoff (`struct iris_boot_info`: memory map + framebuffer, written by the
 * UEFI loader).  This header is the KERNEL→ROOT-TASK handoff: a structured,
 * self-describing description of the capabilities the kernel installed in the
 * root task's CSpace before it ever ran.
 *
 * The kernel writes one page of this and maps it read-only, non-executable
 * into the root task's address space; the virtual address arrives in RBX
 * (the register that used to carry a bootstrap HANDLE, and carried 0 from the
 * close of Stage 4 until this page existed).
 *
 * THE PAGE IS NOT AUTHORITY.  Charter §3.5 forbids an address standing in for
 * a capability, and nothing here does: every `cptr` field names a slot the
 * kernel already populated in the root CNode.  Reading the page tells the root
 * task what it holds; it confers nothing.  A root task that ignores the page
 * still holds exactly the same authority, and one that fabricates a CPtr out
 * of it gets whatever that slot really contains — which is nothing, unless the
 * kernel put something there.  seL4's BootInfo frame is the same shape and the
 * same non-claim.
 *
 * What it REPLACES is a compile-time convention: the root task used to know
 * its untypeds by the constant BOOT_CPTR_UNTYPED_START and discover how many
 * it had by invoking slots until one answered NOT_FOUND — i.e. by probing an
 * address space it was never told the shape of.  A layout agreed by two
 * separately-compiled artifacts is not a contract; it is a coincidence that
 * holds until one of them changes.
 *
 * Versioning: a reader validates `magic` and `version` and refuses the page
 * otherwise — it must never guess.  `header_bytes` is the offset of
 * `untyped[0]`, so a reader built against an older version can still find the
 * array when the header grows (prefix compatibility, the same rule as the
 * versioned user-buffer ABI of checkpoint C.1).
 */

#define IRIS_ROOT_BOOTINFO_MAGIC   0x49524953524F4F54ULL  /* "IRISROOT" */
#define IRIS_ROOT_BOOTINFO_VERSION 3u

/* Size of the region the kernel maps.  Two pages, and the reason is a rule
 * rather than a round number: the description must be able to cover every
 * capability the kernel can grant.  A root CNode has 256 slots, of which
 * 240 can hold boot untypeds; one page describes 126 of them, so a one-page
 * BootInfo would silently cap the memory handed to userland at whatever fits
 * in the page.  root_bootinfo.c static-asserts the relation, so growing the
 * root CNode or the descriptor is a build failure, not a boot-time surprise. */
#define IRIS_ROOT_BOOTINFO_PAGES 2u
#define IRIS_ROOT_BOOTINFO_BYTES (IRIS_ROOT_BOOTINFO_PAGES * 4096u)

/* One untyped region the root task owns, and the slot it owns it in. */
struct iris_bootinfo_untyped {
    uint64_t cptr;        /* CPtr of the KUntyped cap in the root CNode */
    uint64_t paddr;       /* physical base of the region */
    uint64_t size_bytes;  /* region size */
    uint32_t is_device;   /* 0 = RAM, 1 = device memory */
    uint32_t reserved;    /* 0 */
};

struct iris_root_bootinfo {
    uint64_t magic;         /* IRIS_ROOT_BOOTINFO_MAGIC */
    uint32_t version;       /* IRIS_ROOT_BOOTINFO_VERSION */
    uint32_t header_bytes;  /* offset of untyped[0] */
    uint64_t total_bytes;   /* bytes the kernel wrote, header included */

    /* Initial capabilities, named by the CPtr the kernel minted them into.
     * A zero CPtr means "not granted" — never "look somewhere else". */
    uint64_t cap_bootstrap;      /* what is LEFT of the monolith: spawn and
                                  * framebuffer authority (Etapa 2 is still
                                  * splitting these out) */
    uint64_t cap_vspace;         /* the root task's own KVSpace */
    uint64_t cap_irq_control;    /* v2: SYS_CAP_CREATE_IRQCAP authority, alone */
    uint64_t cap_ioport_control; /* v2: SYS_CAP_CREATE_IOPORT authority, alone */
    uint64_t cap_debug_control;  /* v3: klog drain / sched info / poweroff */

    /* The CSpace as it was handed over. */
    uint32_t cnode_slots;      /* slot count of the root CNode */
    uint32_t empty_slot_first; /* first slot the kernel left empty */
    uint32_t empty_slot_end;   /* one past the last (== cnode_slots) */
    uint32_t untyped_count;    /* entries in untyped[] */

    struct iris_bootinfo_untyped untyped[];
};

#ifdef __KERNEL__

/* How many untyped descriptors fit in a buffer of `bytes` bytes. */
uint32_t root_bootinfo_capacity(uint32_t bytes);

/* Write the header into `buf`; every header field is written, so the caller
 * need not pre-zero it.  The empty range starts out EMPTY (first == end ==
 * cnode_slots): the boot path declares free slots only once it knows which
 * ones it did not use.  IRIS_ERR_INVALID_ARG if the buffer cannot even hold
 * the header. */
iris_error_t root_bootinfo_init(void *buf, uint32_t bytes,
                                uint64_t cap_bootstrap, uint64_t cap_vspace,
                                uint32_t cnode_slots);

/* Record a control capability the boot path published.  Called once per
 * authority; `kind` is an IRIS_BOOTCAP_* value and selects the field. */
iris_error_t root_bootinfo_set_control_cap(void *buf, uint32_t bytes,
                                           uint32_t kind, uint64_t cptr);

/* Declare the slot range the kernel left empty, validated against the
 * cnode_slots recorded by init: first <= end <= cnode_slots. */
iris_error_t root_bootinfo_set_empty_range(void *buf, uint32_t bytes,
                                           uint32_t first, uint32_t end);

/* Append one untyped descriptor.  IRIS_ERR_NO_MEMORY when the buffer is full —
 * the caller must then stop granting untypeds, because a capability the root
 * task is not told about is a capability it cannot use. */
iris_error_t root_bootinfo_add_untyped(void *buf, uint32_t bytes,
                                       uint64_t cptr, uint64_t paddr,
                                       uint64_t size_bytes, int is_device);

#endif /* __KERNEL__ */

#endif
