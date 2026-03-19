#include <efi.h>
#include <efilib.h>
#include <iris/kernel.h>

void iris_kernel_main(void) {
    Print(L"====================================\r\n");
    Print(L"        IRIS KERNEL STAGE 1         \r\n");
    Print(L"====================================\r\n");
    Print(L"[IRIS][KERNEL] separate kernel image loaded\r\n");
    Print(L"[IRIS][KERNEL] execution transferred successfully\r\n");
}
