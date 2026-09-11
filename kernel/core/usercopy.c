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

int copy_to_user_checked(uint64_t dst_uptr, const void *src, uint32_t len) {
    uint8_t *d = (uint8_t *)(uintptr_t)dst_uptr;
    const uint8_t *s = (const uint8_t *)src;

    if (!src || !user_range_writable(dst_uptr, len)) return 0;
    user_access_begin();
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
    user_access_end();
    return 1;
}
