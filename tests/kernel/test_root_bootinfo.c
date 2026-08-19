/*
 * test_root_bootinfo.c — Stage 5, Etapa 1: the root task's BootInfo builder.
 *
 * The builder writes the page the root task reads to learn what the kernel put
 * in its CSpace.  Two failure modes matter and neither is observable from a
 * successful boot:
 *
 *   • writing past the buffer — the page is mapped into ring 3, so an overrun
 *     is a kernel memory bug that hands the overrun to userland;
 *   • describing a CSpace that is not the one that was built — a root task
 *     that trusts a wrong descriptor invokes a slot that holds something else,
 *     or skips one that holds a capability.
 *
 * Both are buffer arithmetic, so they are tested here on the host rather than
 * inferred from a boot that happened to work on this machine's memory map.
 *
 *   [RBI-1]  capacity is derived from the buffer, and is 0 below the header
 *   [RBI-2]  init writes a well-formed header with an EMPTY empty-range
 *   [RBI-3]  init refuses a buffer that cannot hold the header
 *   [RBI-4]  descriptors append in order and track total_bytes
 *   [RBI-5]  the array stops at capacity — it never overruns the buffer
 *   [RBI-6]  a descriptor naming CPTR_NULL or a zero-sized region is refused
 *   [RBI-7]  add on an uninitialised / wrong-magic buffer is refused
 *   [RBI-8]  the empty range is validated against the CNode it describes
 *   [RBI-9]  the mapped region describes every untyped a 256-slot root holds
 *   [RBI-10] total_bytes never exceeds the buffer, at any fill level
 *   [RBI-11] each control capability lands in its own field; an unknown kind
 *            or a CPTR_NULL grant is refused
 */
#include "framework.h"
#include <iris/root_bootinfo.h>
#include <iris/nc/error.h>
#include <iris/nc/kbootcap.h>
#include <iris/nc/kcnode.h>
#include <iris/boot_info.h>
#include <string.h>
#include <stdlib.h>

#define RBI_PAGE IRIS_ROOT_BOOTINFO_BYTES

static struct iris_root_bootinfo *rbi_buf(unsigned char *raw) {
    return (struct iris_root_bootinfo *)raw;
}

void test_root_bootinfo(void) {
    TEST_SUITE("root bootinfo (Stage 5)");

    unsigned char *page = (unsigned char *)malloc(RBI_PAGE);
    ASSERT_NOT_NULL(page);
    if (!page) return;

    /* ── [RBI-1] capacity ─────────────────────────────────────────── */
    {
        uint32_t hdr = (uint32_t)sizeof(struct iris_root_bootinfo);
        uint32_t ent = (uint32_t)sizeof(struct iris_bootinfo_untyped);

        ASSERT_EQ(root_bootinfo_capacity(0u), 0u);
        ASSERT_EQ(root_bootinfo_capacity(hdr - 1u), 0u);
        ASSERT_EQ(root_bootinfo_capacity(hdr), 0u);
        ASSERT_EQ(root_bootinfo_capacity(hdr + ent), 1u);
        ASSERT_EQ(root_bootinfo_capacity(hdr + ent + (ent - 1u)), 1u);
        ASSERT_EQ(root_bootinfo_capacity(RBI_PAGE), (RBI_PAGE - hdr) / ent);
    }

    /* ── [RBI-2] init header ──────────────────────────────────────── */
    {
        memset(page, 0xAA, RBI_PAGE);
        ASSERT_EQ(root_bootinfo_init(page, RBI_PAGE, BOOT_CPTR_VSPACE,
                                     KCNODE_DEFAULT_SLOTS),
                  IRIS_OK);

        struct iris_root_bootinfo *bi = rbi_buf(page);
        ASSERT_EQ(bi->magic, IRIS_ROOT_BOOTINFO_MAGIC);
        ASSERT_EQ(bi->version, IRIS_ROOT_BOOTINFO_VERSION);
        ASSERT_EQ(bi->header_bytes, (uint32_t)sizeof(*bi));
        ASSERT_EQ(bi->total_bytes, (uint64_t)sizeof(*bi));
        ASSERT_EQ(bi->cap_vspace, (uint64_t)BOOT_CPTR_VSPACE);
        /* Control capabilities are recorded one by one as the boot path
         * publishes them; init claims none of them. */
        ASSERT_EQ(bi->cap_irq_control, 0u);
        ASSERT_EQ(bi->cap_ioport_control, 0u);
        ASSERT_EQ(bi->cap_debug_control, 0u);
        ASSERT_EQ(bi->cap_proc_control, 0u);
        ASSERT_EQ(bi->cap_initrd_control, 0u);
        ASSERT_EQ(bi->cap_fb_control, 0u);
        ASSERT_EQ(bi->cnode_slots, (uint32_t)KCNODE_DEFAULT_SLOTS);
        ASSERT_EQ(bi->untyped_count, 0u);
        /* No slot is claimed free until the boot path says so. */
        ASSERT_EQ(bi->empty_slot_first, bi->empty_slot_end);
        ASSERT_EQ(bi->empty_slot_end, (uint32_t)KCNODE_DEFAULT_SLOTS);
    }

    /* ── [RBI-3] buffer too small for the header ──────────────────── */
    {
        ASSERT_EQ(root_bootinfo_init(page, (uint32_t)sizeof(struct iris_root_bootinfo) - 1u,
                                     2u, 256u),
                  IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(root_bootinfo_init(NULL, RBI_PAGE, 2u, 256u),
                  IRIS_ERR_INVALID_ARG);
    }

    /* ── [RBI-4] descriptors append in order ──────────────────────── */
    {
        memset(page, 0, RBI_PAGE);
        ASSERT_EQ(root_bootinfo_init(page, RBI_PAGE, 2u, 256u), IRIS_OK);

        struct iris_root_bootinfo *bi = rbi_buf(page);
        for (uint64_t i = 0; i < 4u; i++)
            ASSERT_EQ(root_bootinfo_add_untyped(page, RBI_PAGE,
                                                BOOT_CPTR_UNTYPED_START + i,
                                                0x100000ULL * (i + 1u),
                                                0x4000ULL * (i + 1u),
                                                (int)(i & 1u)),
                      IRIS_OK);
        ASSERT_EQ(bi->untyped_count, 4u);
        for (uint32_t i = 0; i < 4u; i++) {
            ASSERT_EQ(bi->untyped[i].cptr, (uint64_t)(BOOT_CPTR_UNTYPED_START + i));
            ASSERT_EQ(bi->untyped[i].paddr, 0x100000ULL * (i + 1u));
            ASSERT_EQ(bi->untyped[i].size_bytes, 0x4000ULL * (i + 1u));
            ASSERT_EQ(bi->untyped[i].is_device, (uint32_t)(i & 1u));
            ASSERT_EQ(bi->untyped[i].reserved, 0u);
        }
        ASSERT_EQ(bi->total_bytes,
                  (uint64_t)sizeof(*bi) + 4u * sizeof(struct iris_bootinfo_untyped));
    }

    /* ── [RBI-5] the array stops at capacity ──────────────────────── */
    {
        uint32_t hdr = (uint32_t)sizeof(struct iris_root_bootinfo);
        uint32_t ent = (uint32_t)sizeof(struct iris_bootinfo_untyped);
        uint32_t bytes = hdr + 2u * ent;          /* exactly two descriptors */
        unsigned char guard_page[8192];
        memset(guard_page, 0xEE, sizeof(guard_page));

        ASSERT_EQ(root_bootinfo_init(guard_page, bytes, 2u, 256u), IRIS_OK);
        ASSERT_EQ(root_bootinfo_add_untyped(guard_page, bytes, 16u, 0x1000u, 0x1000u, 0), IRIS_OK);
        ASSERT_EQ(root_bootinfo_add_untyped(guard_page, bytes, 17u, 0x2000u, 0x1000u, 0), IRIS_OK);
        /* Third does not fit — refused, and nothing past `bytes` is touched. */
        ASSERT_EQ(root_bootinfo_add_untyped(guard_page, bytes, 18u, 0x3000u, 0x1000u, 0),
                  IRIS_ERR_NO_MEMORY);
        ASSERT_EQ(rbi_buf(guard_page)->untyped_count, 2u);
        ASSERT_EQ(rbi_buf(guard_page)->total_bytes, (uint64_t)bytes);
        for (uint32_t i = bytes; i < sizeof(guard_page); i++)
            ASSERT_EQ(guard_page[i], 0xEE);
    }

    /* ── [RBI-6] a descriptor must name something invocable ───────── */
    {
        memset(page, 0, RBI_PAGE);
        ASSERT_EQ(root_bootinfo_init(page, RBI_PAGE, 2u, 256u), IRIS_OK);
        ASSERT_EQ(root_bootinfo_add_untyped(page, RBI_PAGE, 0u, 0x1000u, 0x1000u, 0),
                  IRIS_ERR_INVALID_ARG);   /* CPTR_NULL */
        ASSERT_EQ(root_bootinfo_add_untyped(page, RBI_PAGE, 16u, 0x1000u, 0u, 0),
                  IRIS_ERR_INVALID_ARG);   /* empty region */
        ASSERT_EQ(rbi_buf(page)->untyped_count, 0u);
    }

    /* ── [RBI-7] add refuses a buffer it did not write ────────────── */
    {
        memset(page, 0, RBI_PAGE);
        ASSERT_EQ(root_bootinfo_add_untyped(page, RBI_PAGE, 16u, 0x1000u, 0x1000u, 0),
                  IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(root_bootinfo_set_empty_range(page, RBI_PAGE, 0u, 0u),
                  IRIS_ERR_INVALID_ARG);

        ASSERT_EQ(root_bootinfo_init(page, RBI_PAGE, 2u, 256u), IRIS_OK);
        rbi_buf(page)->version = IRIS_ROOT_BOOTINFO_VERSION + 1u;
        ASSERT_EQ(root_bootinfo_add_untyped(page, RBI_PAGE, 16u, 0x1000u, 0x1000u, 0),
                  IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(root_bootinfo_set_empty_range(page, RBI_PAGE, 16u, 256u),
                  IRIS_ERR_INVALID_ARG);
    }

    /* ── [RBI-8] the empty range describes real slots ─────────────── */
    {
        memset(page, 0, RBI_PAGE);
        ASSERT_EQ(root_bootinfo_init(page, RBI_PAGE, 2u, 256u), IRIS_OK);

        ASSERT_EQ(root_bootinfo_set_empty_range(page, RBI_PAGE, 29u, 256u), IRIS_OK);
        ASSERT_EQ(rbi_buf(page)->empty_slot_first, 29u);
        ASSERT_EQ(rbi_buf(page)->empty_slot_end, 256u);

        /* Inverted, and past the end of the CNode. */
        ASSERT_EQ(root_bootinfo_set_empty_range(page, RBI_PAGE, 40u, 30u),
                  IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(root_bootinfo_set_empty_range(page, RBI_PAGE, 16u, 257u),
                  IRIS_ERR_INVALID_ARG);
        /* Refused writes leave the last good range in place. */
        ASSERT_EQ(rbi_buf(page)->empty_slot_first, 29u);
        ASSERT_EQ(rbi_buf(page)->empty_slot_end, 256u);

        /* An empty free range is legal: a root CNode can be fully populated. */
        ASSERT_EQ(root_bootinfo_set_empty_range(page, RBI_PAGE, 256u, 256u), IRIS_OK);
    }

    /* ── [RBI-9] the region covers a 256-slot root CNode ──────────── */
    {
        /* Every slot from BOOT_CPTR_UNTYPED_START to the end of the root CNode
         * must be describable, or the drain would have to stop before the
         * CSpace does — a description that cannot cover the CSpace it hands
         * over silently shrinks the boot budget.  root_bootinfo.c asserts the
         * same relation at build time; this is the runtime witness. */
        ASSERT_TRUE(root_bootinfo_capacity(RBI_PAGE) >=
                    (uint32_t)(KCNODE_DEFAULT_SLOTS - BOOT_CPTR_UNTYPED_START));
    }

    /* ── [RBI-10] total_bytes stays inside the buffer ─────────────── */
    {
        memset(page, 0, RBI_PAGE);
        ASSERT_EQ(root_bootinfo_init(page, RBI_PAGE, 2u, 256u), IRIS_OK);
        uint32_t cap = root_bootinfo_capacity(RBI_PAGE);
        for (uint32_t i = 0; i < cap; i++) {
            ASSERT_EQ(root_bootinfo_add_untyped(page, RBI_PAGE, 16u + i,
                                                0x1000u * (i + 1u), 0x1000u, 0),
                      IRIS_OK);
            ASSERT_TRUE(rbi_buf(page)->total_bytes <= (uint64_t)RBI_PAGE);
        }
        ASSERT_EQ(rbi_buf(page)->untyped_count, cap);
        ASSERT_EQ(root_bootinfo_add_untyped(page, RBI_PAGE, 4000u, 0x1000u, 0x1000u, 0),
                  IRIS_ERR_NO_MEMORY);
    }

    /* ── [RBI-11] control capabilities, one field per authority ───── */
    {
        memset(page, 0, RBI_PAGE);
        ASSERT_EQ(root_bootinfo_init(page, RBI_PAGE, 2u, 256u), IRIS_OK);
        struct iris_root_bootinfo *bi = rbi_buf(page);

        ASSERT_EQ(root_bootinfo_set_control_cap(page, RBI_PAGE,
                      IRIS_BOOTCAP_IRQ_CONTROL, 3u), IRIS_OK);
        ASSERT_EQ(root_bootinfo_set_control_cap(page, RBI_PAGE,
                      IRIS_BOOTCAP_IOPORT_CONTROL, 4u), IRIS_OK);
        ASSERT_EQ(root_bootinfo_set_control_cap(page, RBI_PAGE,
                      IRIS_BOOTCAP_DEBUG_CONTROL, 5u), IRIS_OK);
        ASSERT_EQ(root_bootinfo_set_control_cap(page, RBI_PAGE,
                      IRIS_BOOTCAP_PROC_CONTROL, 6u), IRIS_OK);
        ASSERT_EQ(root_bootinfo_set_control_cap(page, RBI_PAGE,
                      IRIS_BOOTCAP_INITRD_CONTROL, 7u), IRIS_OK);
        ASSERT_EQ(root_bootinfo_set_control_cap(page, RBI_PAGE,
                      IRIS_BOOTCAP_FB_CONTROL, 8u), IRIS_OK);

        /* Each authority has its OWN field: a reader asking for one never
         * gets another, which is the whole point of the split. */
        ASSERT_EQ(bi->cap_irq_control, 3u);
        ASSERT_EQ(bi->cap_ioport_control, 4u);
        ASSERT_EQ(bi->cap_debug_control, 5u);
        ASSERT_EQ(bi->cap_proc_control, 6u);
        ASSERT_EQ(bi->cap_initrd_control, 7u);
        ASSERT_EQ(bi->cap_fb_control, 8u);

        /* A kind the page has no field for, and a grant naming CPTR_NULL, are
         * refused rather than dropped silently. */
        ASSERT_EQ(root_bootinfo_set_control_cap(page, RBI_PAGE, 0u, 9u),
                  IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(root_bootinfo_set_control_cap(page, RBI_PAGE,
                      IRIS_BOOTCAP_IRQ_CONTROL | IRIS_BOOTCAP_DEBUG_CONTROL, 9u),
                  IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(root_bootinfo_set_control_cap(page, RBI_PAGE,
                      IRIS_BOOTCAP_FB_CONTROL, 0u),
                  IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(bi->cap_fb_control, 8u);
    }

    free(page);
}
