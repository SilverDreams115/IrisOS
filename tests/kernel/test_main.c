#include "framework.h"
#include <stdio.h>

int g_pass = 0;
int g_fail = 0;

void test_rights(void);
void test_kobject(void);
void test_kcnode(void);
void test_kuntyped(void);
void test_handle_table(void);
void test_kendpoint(void);
void test_kchannel(void);
void test_knotification(void);
void test_kreply(void);
void test_kschedctx(void);
void test_cspace(void);
void test_ipc_cspace(void);
void test_untyped_cspace(void);
void test_boot_cspace(void);
void test_vspace_cspace(void);
void test_klog(void);

int main(void) {
    test_rights();
    test_kobject();
    test_kcnode();
    test_kuntyped();
    test_handle_table();
    test_kendpoint();
    test_kchannel();
    test_knotification();
    test_kreply();
    test_kschedctx();
    test_cspace();
    test_ipc_cspace();
    test_untyped_cspace();
    test_boot_cspace();
    test_vspace_cspace();
    test_klog();

    printf("\nresult: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
