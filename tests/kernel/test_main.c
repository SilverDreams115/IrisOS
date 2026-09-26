/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "framework.h"
#include <stdio.h>

int g_pass = 0;
int g_fail = 0;

void test_rights(void);
void test_kobject(void);
void test_kcnode(void);
void test_kuntyped(void);
void test_kendpoint(void);
void test_knotification(void);
void test_kreply(void);
void test_kschedctx(void);
void test_cspace(void);
void test_ipc_cspace(void);
void test_untyped_cspace(void);
void test_boot_cspace(void);
void test_vspace_cspace(void);
void test_kasidpool(void);
void test_kframe(void);
void test_mdb(void);
void test_cnode_teardown_depth(void);
void test_mdb_parent_identity(void);
void test_klog(void);
void test_vfs_ep(void);
void test_root_bootinfo(void);
void test_pagetable(void);
void test_cnode_guard(void);
void test_schedctx_refill(void);
void test_syscall_cspace(void);
void test_syscall_retype(void);
void test_syscall_tcb(void);
void test_syscall_ipc(void);
void test_syscall_dispatch(void);
void test_abi(void);

/*
 * A-45 — back the "physical memory" the tests pretend to own.
 *
 * Several suites build an Untyped over a made-up physical address (0x100000,
 * 0x200000, ...) and assert on it, and the host's PHYS_TO_VIRT is the
 * identity.  That was harmless while nothing WROTE through those addresses.
 * Retype now zeroes the page it hands out, the way seL4 does, so the pretend
 * memory has to be real memory -- which is the more faithful harness anyway:
 * the kernel's own PHYS_TO_VIRT always lands in mapped RAM.
 */
static void host_back_fake_physmem(void) {
    void *p = mmap((void *)(uintptr_t)0x100000ull, 0x1000000ull,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "test_main: could not back fake physical memory\n");
        exit(1);
    }
}

int main(void) {
    host_back_fake_physmem();
    test_rights();
    test_kobject();
    test_kcnode();
    test_kuntyped();
    test_kendpoint();
    test_knotification();
    test_kreply();
    test_kschedctx();
    test_cspace();
    test_ipc_cspace();
    test_untyped_cspace();
    test_boot_cspace();
    test_vspace_cspace();
    test_kasidpool();
    test_kframe();
    test_mdb();
    test_cnode_teardown_depth();
    test_mdb_parent_identity();
    test_klog();
    test_vfs_ep();
    test_root_bootinfo();
    test_pagetable();
    test_cnode_guard();
    test_schedctx_refill();
    test_syscall_cspace();
    test_syscall_retype();
    test_syscall_tcb();
    test_syscall_ipc();
    test_syscall_dispatch();
    test_abi();

    printf("\nresult: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
