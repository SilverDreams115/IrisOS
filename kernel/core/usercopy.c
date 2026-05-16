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

int user_range_readable(uint64_t ptr, uint32_t len) {
    return user_range_accessible(ptr, len, 0);
}

int user_range_writable(uint64_t ptr, uint32_t len) {
    return user_range_accessible(ptr, len, PAGE_WRITABLE);
}

int copy_from_user_checked(void *dst, uint64_t src_uptr, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)(uintptr_t)src_uptr;

    if (!dst || !user_range_readable(src_uptr, len)) return 0;
    user_access_begin();
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
    user_access_end();
    return 1;
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

uint32_t copy_user_cstr_bounded(uint64_t uptr, char *dst, uint32_t cap) {
    const char *src = (const char *)(uintptr_t)uptr;
    uint32_t i = 0;

    if (!dst || cap == 0) return 0;

    /* Validate and copy one page-aligned chunk at a time to avoid an
     * O(n) page-table walk per byte.  Each iteration checks exactly the
     * pages spanned by the remaining bytes of the current 4 KiB page. */
    while (i < cap - 1) {
        uint64_t cur_ptr  = uptr + i;
        uint32_t page_rem = (uint32_t)(0x1000ULL - (cur_ptr & 0xFFFULL));
        uint32_t avail    = cap - 1 - i;
        uint32_t chunk    = (page_rem < avail) ? page_rem : avail;

        if (!user_range_readable(cur_ptr, chunk)) break;

        user_access_begin();
        for (uint32_t j = 0; j < chunk; j++) {
            char c = src[i + j];
            if (!c) { user_access_end(); dst[i + j] = '\0'; return i + j; }
            dst[i + j] = c;
        }
        user_access_end();
        i += chunk;
    }
    dst[i] = '\0';
    return i;
}
