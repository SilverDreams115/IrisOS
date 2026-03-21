#include <iris/syscall.h>
#include <iris/task.h>

/* MSR addresses */
#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

extern void syscall_entry(void);

#include <iris/serial.h>
#include <iris/paging.h>
#include <iris/vfs.h>

/* user virtual address space bounds */
#define USER_ADDR_MIN 0x1000ULL
#define USER_ADDR_MAX 0x400000ULL

/* Check that [ptr, ptr+len) is within user range AND mapped in page table.
 * Every page that overlaps the range is checked individually. */
static int user_ptr_valid(uint64_t ptr, uint32_t len) {
    if (ptr == 0) return 0;
    if (ptr < USER_ADDR_MIN) return 0;
    if (len == 0) return 0;
    if (ptr + (uint64_t)len > USER_ADDR_MAX) return 0;
    if (ptr + (uint64_t)len < ptr) return 0; /* overflow check */
    /* verify every page in range is present in the page table */
    uint64_t page = ptr & ~0xFFFULL;
    uint64_t end  = (ptr + len - 1) & ~0xFFFULL;
    for (; page <= end; page += 0x1000ULL) {
        if (paging_virt_to_phys(page) == 0) return 0;
    }
    return 1;
}

static uint64_t sys_write(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    /* arg0 = pointer to string in user space */
    if (!user_ptr_valid(arg0, 1)) return (uint64_t)-1;
    const char *s = (const char *)(uintptr_t)arg0;
    /* safe string print — max 256 chars */
    char buf[257];
    uint32_t i = 0;
    while (i < 256 && user_ptr_valid(arg0 + i, 1) && s[i]) {
        buf[i] = s[i];
        i++;
    }
    buf[i] = '\0';
    serial_write("[USER] ");
    serial_write(buf);
    return i;
}

static uint64_t sys_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    serial_write("[SYSCALL] exit code=");
    serial_write_dec(arg0);
    serial_write("\n");
    /* mark current task dead and yield forever */
    struct task *t = task_current();
    if (t) t->state = TASK_DEAD;
    /* loop yielding — sysretq will NOT be executed after this */
    for (;;) task_yield();
    return 0; /* unreachable */
}

static uint64_t sys_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    task_yield();
    return 0;
}

static uint64_t sys_getpid(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    return t ? t->id : 0;
}

/* per-task brk heap pointer — simple bump allocator base */
#define USER_HEAP_BASE  0x100000ULL
#define USER_HEAP_MAX   0x200000ULL
static uint64_t brk_current = USER_HEAP_BASE;

static uint64_t sys_open(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    if (!user_ptr_valid(arg0, 1)) return (uint64_t)-1;
    const char *path = (const char *)(uintptr_t)arg0;
    /* copy path safely */
    char buf[VFS_MAX_NAME];
    uint32_t i = 0;
    while (i < VFS_MAX_NAME - 1 && user_ptr_valid(arg0 + i, 1) && path[i]) {
        buf[i] = path[i]; i++;
    }
    buf[i] = '\0';
    uint32_t flags = (uint32_t)arg1;
    int32_t fd = vfs_open(buf, flags);
    return (uint64_t)(int64_t)fd;
}

static uint64_t sys_read(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    int32_t  fd  = (int32_t)arg0;
    uint64_t buf = arg1;
    uint32_t len = (uint32_t)arg2;
    if (!user_ptr_valid(buf, len)) return (uint64_t)-1;
    int32_t n = vfs_read(fd, (void *)(uintptr_t)buf, len);
    return (uint64_t)(int64_t)n;
}

static uint64_t sys_close(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    int32_t fd = (int32_t)arg0;
    return (uint64_t)(int64_t)vfs_close(fd);
}

static uint64_t sys_brk(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    /* arg0 = requested new brk; 0 = query current */
    if (arg0 == 0) return brk_current;
    if (arg0 < USER_HEAP_BASE) return brk_current;
    if (arg0 > USER_HEAP_MAX)  return brk_current;
    brk_current = arg0;
    return brk_current;
}

static uint64_t sys_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    /* arg0 = ticks to sleep (at 100 Hz, 1 tick = 10ms) */
    uint64_t ticks = arg0;
    for (uint64_t i = 0; i < ticks; i++)
        task_yield();
    return 0;
}

uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,

                          uint64_t arg1, uint64_t arg2) {
    switch (num) {
        case SYS_WRITE:  return sys_write(arg0, arg1, arg2);
        case SYS_EXIT:   return sys_exit(arg0, arg1, arg2);
        case SYS_GETPID: return sys_getpid(arg0, arg1, arg2);
        case SYS_YIELD:  return sys_yield(arg0, arg1, arg2);
        case SYS_OPEN:   return sys_open(arg0, arg1, arg2);
        case SYS_READ:   return sys_read(arg0, arg1, arg2);
        case SYS_CLOSE:  return sys_close(arg0, arg1, arg2);
        case SYS_BRK:    return sys_brk(arg0, arg1, arg2);
        case SYS_SLEEP:  return sys_sleep(arg0, arg1, arg2);
        default:
            serial_write("[SYSCALL] unknown syscall=");
            serial_write_dec(num);
            serial_write("\n");
            return (uint64_t)-1;
    }
}

/* syscall_kstack_ptr lives in syscall_entry.S .data section */
extern uint64_t syscall_kstack_ptr;

void syscall_set_kstack(uint64_t kstack_top) {
    syscall_kstack_ptr = kstack_top;
}

void syscall_init(void) {
    /* enable SCE bit in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= (1ULL << 0); /* SCE = syscall enable */
    wrmsr(MSR_EFER, efer);

    /* STAR: bits 63:48 = user CS-8 (sysret CS = this+16, SS = this+8)
     *       bits 47:32 = kernel CS (syscall CS, SS = this+8)
     * kernel CS = 0x08, kernel SS = 0x10
     * user   CS = 0x1B (0x18|3), user SS = 0x23 (0x20|3)
     * STAR[47:32] = 0x0008  (kernel: CS=0x08, SS=0x10)
     * STAR[63:48] = 0x0013  (sysret: CS=0x1B=0x13|3, SS=0x23=0x1B+8)
     */
    uint64_t star = 0;
    star |= ((uint64_t)0x0008 << 32); /* kernel CS selector */
    star |= ((uint64_t)0x0013 << 48); /* user CS-8 for sysret */
    wrmsr(MSR_STAR, star);

    /* LSTAR: syscall handler entry point */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* SFMASK: clear IF on syscall entry (disable interrupts) */
    wrmsr(MSR_SFMASK, (1ULL << 9)); /* IF = bit 9 */
}
