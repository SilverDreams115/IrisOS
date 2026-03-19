#include <efi.h>
#include <efilib.h>
#include <iris/kernel.h>

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);

    Print(L"[IRIS][KERNEL] efi entry OK\r\n");
    iris_kernel_main();

    return EFI_SUCCESS;
}
