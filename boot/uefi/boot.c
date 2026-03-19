#include <efi.h>
#include <efilib.h>
#include <elf.h>
#include <stdint.h>
#include <iris/boot_info.h>

#define IRIS_KERNEL_PATH L"\\EFI\\IRIS\\KERNEL.ELF"
#define IRIS_PAGE_SIZE 4096ULL

static void *mem_copy(void *dst, const void *src, UINTN size) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;

    while (size--) {
        *d++ = *s++;
    }

    return dst;
}

static void *mem_set(void *dst, UINT8 value, UINTN size) {
    UINT8 *d = (UINT8 *)dst;

    while (size--) {
        *d++ = value;
    }

    return dst;
}

static UINT64 align_down(UINT64 value, UINT64 align) {
    return value & ~(align - 1);
}

static UINTN page_count(UINT64 size) {
    return (UINTN)((size + IRIS_PAGE_SIZE - 1) / IRIS_PAGE_SIZE);
}

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

static EFI_STATUS fill_framebuffer_info(
    EFI_SYSTEM_TABLE *SystemTable,
    struct iris_boot_info *BootInfo
) {
    EFI_STATUS Status;
    EFI_GUID GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;

    BootInfo->framebuffer.base = 0;
    BootInfo->framebuffer.size = 0;
    BootInfo->framebuffer.width = 0;
    BootInfo->framebuffer.height = 0;
    BootInfo->framebuffer.pixels_per_scanline = 0;
    BootInfo->framebuffer.reserved = 0;

    Status = uefi_call_wrapper(
        SystemTable->BootServices->LocateProtocol,
        3,
        &GopGuid,
        NULL,
        (VOID **)&Gop
    );
    if (EFI_ERROR(Status) || Gop == NULL || Gop->Mode == NULL || Gop->Mode->Info == NULL) {
        Print(L"[IRIS][LOADER] GOP unavailable\r\n");
        return Status;
    }

    BootInfo->framebuffer.base = (uint64_t)Gop->Mode->FrameBufferBase;
    BootInfo->framebuffer.size = (uint64_t)Gop->Mode->FrameBufferSize;
    BootInfo->framebuffer.width = (uint32_t)Gop->Mode->Info->HorizontalResolution;
    BootInfo->framebuffer.height = (uint32_t)Gop->Mode->Info->VerticalResolution;
    BootInfo->framebuffer.pixels_per_scanline = (uint32_t)Gop->Mode->Info->PixelsPerScanLine;

    return EFI_SUCCESS;
}

static EFI_STATUS load_elf_kernel(
    EFI_SYSTEM_TABLE *SystemTable,
    VOID *KernelBuffer,
    UINTN KernelSize,
    struct iris_boot_info *BootInfo
) {
    Elf64_Ehdr *Ehdr = (Elf64_Ehdr *)KernelBuffer;
    Elf64_Phdr *Phdrs;
    UINT16 Index;

    if (KernelSize < sizeof(Elf64_Ehdr)) {
        Print(L"[IRIS][LOADER] ELF file too small\r\n");
        return EFI_LOAD_ERROR;
    }

    if (Ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        Ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        Ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        Ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        Print(L"[IRIS][LOADER] invalid ELF magic\r\n");
        return EFI_UNSUPPORTED;
    }

    if (Ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        Ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        Ehdr->e_machine != EM_X86_64) {
        Print(L"[IRIS][LOADER] unsupported ELF format\r\n");
        return EFI_UNSUPPORTED;
    }

    if (Ehdr->e_phoff == 0 ||
        Ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        Ehdr->e_phnum == 0) {
        Print(L"[IRIS][LOADER] invalid ELF program header table\r\n");
        return EFI_LOAD_ERROR;
    }

    Phdrs = (Elf64_Phdr *)((UINT8 *)KernelBuffer + Ehdr->e_phoff);

    for (Index = 0; Index < Ehdr->e_phnum; ++Index) {
        Elf64_Phdr *Phdr = &Phdrs[Index];
        EFI_PHYSICAL_ADDRESS SegmentBase;
        EFI_PHYSICAL_ADDRESS AllocBase;
        UINT64 SegmentOffsetInPage;
        UINTN Pages;
        VOID *CopyDst;
        EFI_STATUS Status;

        if (Phdr->p_type != PT_LOAD) {
            continue;
        }

        if (Phdr->p_offset + Phdr->p_filesz > KernelSize) {
            Print(L"[IRIS][LOADER] ELF segment exceeds file size\r\n");
            return EFI_LOAD_ERROR;
        }

        SegmentBase = (EFI_PHYSICAL_ADDRESS)(Phdr->p_paddr ? Phdr->p_paddr : Phdr->p_vaddr);
        AllocBase = (EFI_PHYSICAL_ADDRESS)align_down(SegmentBase, IRIS_PAGE_SIZE);
        SegmentOffsetInPage = (UINT64)(SegmentBase - AllocBase);
        Pages = page_count(SegmentOffsetInPage + Phdr->p_memsz);

        Status = uefi_call_wrapper(
            SystemTable->BootServices->AllocatePages,
            4,
            AllocateAddress,
            EfiLoaderData,
            Pages,
            &AllocBase
        );
        if (EFI_ERROR(Status)) {
            Print(L"[IRIS][LOADER] AllocatePages failed for segment: %r\r\n", Status);
            return Status;
        }

        CopyDst = (VOID *)(UINTN)SegmentBase;

        mem_copy(
            CopyDst,
            (UINT8 *)KernelBuffer + Phdr->p_offset,
            (UINTN)Phdr->p_filesz
        );

        if (Phdr->p_memsz > Phdr->p_filesz) {
            mem_set(
                (UINT8 *)CopyDst + Phdr->p_filesz,
                0,
                (UINTN)(Phdr->p_memsz - Phdr->p_filesz)
            );
        }
    }

    fill_framebuffer_info(SystemTable, BootInfo);

    {
        void (*KernelEntry)(struct iris_boot_info *) = (void (*)(struct iris_boot_info *))(UINTN)Ehdr->e_entry;
        Print(L"[IRIS][LOADER] transferring control to ELF kernel...\r\n");
        KernelEntry(BootInfo);
    }

    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    VOID *KernelBuffer = NULL;
    UINTN KernelSize = 0;
    static struct iris_boot_info BootInfo;

    InitializeLib(ImageHandle, SystemTable);
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);

    Print(L"====================================\r\n");
    Print(L"        IRIS STAGE 2 LOADER         \r\n");
    Print(L"====================================\r\n");
    Print(L"[IRIS][LOADER] UEFI entry OK\r\n");
    Print(L"[IRIS][LOADER] loading ELF kernel...\r\n");

    BootInfo.magic = IRIS_BOOTINFO_MAGIC;
    BootInfo.version = IRIS_BOOTINFO_VERSION;
    BootInfo.framebuffer.base = 0;
    BootInfo.framebuffer.size = 0;
    BootInfo.framebuffer.width = 0;
    BootInfo.framebuffer.height = 0;
    BootInfo.framebuffer.pixels_per_scanline = 0;
    BootInfo.framebuffer.reserved = 0;

    Status = load_file_into_memory(
        ImageHandle,
        SystemTable,
        IRIS_KERNEL_PATH,
        &KernelBuffer,
        &KernelSize
    );
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] kernel file load failed: %r\r\n", Status);
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 3000000);
        return Status;
    }

    Print(L"[IRIS][LOADER] ELF file read into memory\r\n");

    Status = load_elf_kernel(
        SystemTable,
        KernelBuffer,
        KernelSize,
        &BootInfo
    );

    if (KernelBuffer != NULL) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, KernelBuffer);
    }

    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] ELF kernel load failed: %r\r\n", Status);
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 3000000);
        return Status;
    }

    Print(L"[IRIS][LOADER] unexpected return from ELF kernel\r\n");
    uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 3000000);
    return EFI_SUCCESS;
}
