#include <efi.h>
#include <efilib.h>

#define IRIS_KERNEL_PATH L"\\EFI\\IRIS\\KERNELX64.EFI"

static EFI_STATUS load_file_into_memory(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable,
    CHAR16 *Path,
    VOID **Buffer,
    UINTN *BufferSize
) {
    EFI_STATUS Status;
    EFI_GUID LoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID SimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID FileInfoGuid = EFI_FILE_INFO_ID;

    EFI_LOADED_IMAGE *LoadedImage = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem = NULL;
    EFI_FILE_HANDLE Root = NULL;
    EFI_FILE_HANDLE File = NULL;
    EFI_FILE_INFO *FileInfo = NULL;
    UINTN FileInfoSize = SIZE_OF_EFI_FILE_INFO + 256;

    *Buffer = NULL;
    *BufferSize = 0;

    Status = uefi_call_wrapper(
        SystemTable->BootServices->HandleProtocol,
        3,
        ImageHandle,
        &LoadedImageProtocolGuid,
        (VOID **)&LoadedImage
    );
    if (EFI_ERROR(Status) || LoadedImage == NULL) {
        Print(L"[IRIS][LOADER] HandleProtocol LoadedImage failed: %r\r\n", Status);
        return Status;
    }

    Status = uefi_call_wrapper(
        SystemTable->BootServices->HandleProtocol,
        3,
        LoadedImage->DeviceHandle,
        &SimpleFileSystemProtocolGuid,
        (VOID **)&FileSystem
    );
    if (EFI_ERROR(Status) || FileSystem == NULL) {
        Print(L"[IRIS][LOADER] HandleProtocol SimpleFS failed: %r\r\n", Status);
        return Status;
    }

    Status = uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &Root);
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] OpenVolume failed: %r\r\n", Status);
        return Status;
    }

    Status = uefi_call_wrapper(
        Root->Open,
        5,
        Root,
        &File,
        Path,
        EFI_FILE_MODE_READ,
        0
    );
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] open kernel file failed: %r\r\n", Status);
        uefi_call_wrapper(Root->Close, 1, Root);
        return Status;
    }

    Status = uefi_call_wrapper(
        SystemTable->BootServices->AllocatePool,
        3,
        EfiLoaderData,
        FileInfoSize,
        (VOID **)&FileInfo
    );
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] AllocatePool(FileInfo) failed: %r\r\n", Status);
        uefi_call_wrapper(File->Close, 1, File);
        uefi_call_wrapper(Root->Close, 1, Root);
        return Status;
    }

    Status = uefi_call_wrapper(
        File->GetInfo,
        4,
        File,
        &FileInfoGuid,
        &FileInfoSize,
        FileInfo
    );
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] GetInfo failed: %r\r\n", Status);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, FileInfo);
        uefi_call_wrapper(File->Close, 1, File);
        uefi_call_wrapper(Root->Close, 1, Root);
        return Status;
    }

    *BufferSize = (UINTN)FileInfo->FileSize;

    Status = uefi_call_wrapper(
        SystemTable->BootServices->AllocatePool,
        3,
        EfiLoaderData,
        *BufferSize,
        Buffer
    );
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] AllocatePool(kernel buffer) failed: %r\r\n", Status);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, FileInfo);
        uefi_call_wrapper(File->Close, 1, File);
        uefi_call_wrapper(Root->Close, 1, Root);
        return Status;
    }

    Status = uefi_call_wrapper(File->Read, 3, File, BufferSize, *Buffer);
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] Read failed: %r\r\n", Status);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, *Buffer);
        *Buffer = NULL;
        *BufferSize = 0;
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, FileInfo);
        uefi_call_wrapper(File->Close, 1, File);
        uefi_call_wrapper(Root->Close, 1, Root);
        return Status;
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, FileInfo);
    uefi_call_wrapper(File->Close, 1, File);
    uefi_call_wrapper(Root->Close, 1, Root);

    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    VOID *KernelBuffer = NULL;
    UINTN KernelSize = 0;
    EFI_HANDLE KernelImageHandle = NULL;

    InitializeLib(ImageHandle, SystemTable);
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);

    Print(L"====================================\r\n");
    Print(L"         IRIS STAGE 1 LOADER        \r\n");
    Print(L"====================================\r\n");
    Print(L"[IRIS][LOADER] UEFI entry OK\r\n");
    Print(L"[IRIS][LOADER] loading kernel image...\r\n");

    Status = load_file_into_memory(
        ImageHandle,
        SystemTable,
        IRIS_KERNEL_PATH,
        &KernelBuffer,
        &KernelSize
    );
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] kernel load failed: %r\r\n", Status);
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 3000000);
        return Status;
    }

    Print(L"[IRIS][LOADER] kernel file read into memory\r\n");

    Status = uefi_call_wrapper(
        SystemTable->BootServices->LoadImage,
        6,
        FALSE,
        ImageHandle,
        NULL,
        KernelBuffer,
        KernelSize,
        &KernelImageHandle
    );
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] LoadImage failed: %r\r\n", Status);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, KernelBuffer);
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 3000000);
        return Status;
    }

    Print(L"[IRIS][LOADER] kernel image loaded, starting...\r\n");

    Status = uefi_call_wrapper(
        SystemTable->BootServices->StartImage,
        3,
        KernelImageHandle,
        NULL,
        NULL
    );

    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] StartImage failed: %r\r\n", Status);
    } else {
        Print(L"[IRIS][LOADER] kernel returned control\r\n");
    }

    if (KernelBuffer != NULL) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, KernelBuffer);
    }

    uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 3000000);
    return Status;
}
