#include <efi.h>
#include <efilib.h>
#include <elf.h>
#include <stdint.h>
#include <iris/boot_info.h>

#define IRIS_KERNEL_PATH  L"\\EFI\\IRIS\\KERNEL.ELF"
#define IRIS_PAGE_SIZE    4096ULL

static void *mem_copy(void *dst, const void *src, UINTN size) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    while (size--) *d++ = *s++;
    return dst;
}

static void *mem_set(void *dst, UINT8 value, UINTN size) {
    UINT8 *d = (UINT8 *)dst;
    while (size--) *d++ = value;
    return dst;
}

static UINT64 align_down(UINT64 value, UINT64 align) {
    return value & ~(align - 1);
}

static UINTN page_count(UINT64 size) {
    return (UINTN)((size + IRIS_PAGE_SIZE - 1) / IRIS_PAGE_SIZE);
}

static uint32_t efi_type_to_iris(UINT32 efi_type) {
    switch (efi_type) {
        case EfiConventionalMemory:  return IRIS_MEM_USABLE;
        case EfiLoaderCode:
        case EfiLoaderData:          return IRIS_MEM_BOOTLOADER;
        case EfiBootServicesCode:
        case EfiBootServicesData:    return IRIS_MEM_USABLE;
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData: return IRIS_MEM_RESERVED;
        case EfiACPIReclaimMemory:   return IRIS_MEM_ACPI_RECLAIMABLE;
        case EfiACPIMemoryNVS:       return IRIS_MEM_ACPI_NVS;
        case EfiUnusableMemory:      return IRIS_MEM_BAD;
        default:                     return IRIS_MEM_RESERVED;
    }
}

static EFI_STATUS load_file_into_memory(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable,
    CHAR16 *Path,
    VOID **Buffer,
    UINTN *BufferSize
) {
    EFI_STATUS Status;
    EFI_GUID LoadedImageProtocolGuid      = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID SimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID FileInfoGuid                 = EFI_FILE_INFO_ID;
    EFI_LOADED_IMAGE                *LoadedImage = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem  = NULL;
    EFI_FILE_HANDLE                  Root        = NULL;
    EFI_FILE_HANDLE                  File        = NULL;
    EFI_FILE_INFO                   *FileInfo    = NULL;
    UINTN FileInfoSize = SIZE_OF_EFI_FILE_INFO + 256;

    *Buffer = NULL;
    *BufferSize = 0;

    Status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
        ImageHandle, &LoadedImageProtocolGuid, (VOID **)&LoadedImage);
    if (EFI_ERROR(Status) || !LoadedImage) {
        Print(L"[IRIS][LOADER] HandleProtocol LoadedImage failed: %r\r\n", Status);
        return Status;
    }
    Status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
        LoadedImage->DeviceHandle, &SimpleFileSystemProtocolGuid, (VOID **)&FileSystem);
    if (EFI_ERROR(Status) || !FileSystem) {
        Print(L"[IRIS][LOADER] HandleProtocol SimpleFS failed: %r\r\n", Status);
        return Status;
    }
    Status = uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &Root);
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] OpenVolume failed: %r\r\n", Status);
        return Status;
    }
    Status = uefi_call_wrapper(Root->Open, 5, Root, &File, Path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] open kernel file failed: %r\r\n", Status);
        uefi_call_wrapper(Root->Close, 1, Root);
        return Status;
    }
    Status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
        EfiLoaderData, FileInfoSize, (VOID **)&FileInfo);
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] AllocatePool(FileInfo) failed: %r\r\n", Status);
        uefi_call_wrapper(File->Close, 1, File);
        uefi_call_wrapper(Root->Close, 1, Root);
        return Status;
    }
    Status = uefi_call_wrapper(File->GetInfo, 4, File, &FileInfoGuid, &FileInfoSize, FileInfo);
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] GetInfo failed: %r\r\n", Status);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, FileInfo);
        uefi_call_wrapper(File->Close, 1, File);
        uefi_call_wrapper(Root->Close, 1, Root);
        return Status;
    }
    *BufferSize = (UINTN)FileInfo->FileSize;
    Status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
        EfiLoaderData, *BufferSize, Buffer);
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
        *Buffer = NULL; *BufferSize = 0;
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

static void fill_framebuffer_info(
    EFI_SYSTEM_TABLE *SystemTable,
    struct iris_boot_info *BootInfo
) {
    EFI_STATUS Status;
    EFI_GUID GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;

    BootInfo->framebuffer.base              = 0;
    BootInfo->framebuffer.size              = 0;
    BootInfo->framebuffer.width             = 0;
    BootInfo->framebuffer.height            = 0;
    BootInfo->framebuffer.pixels_per_scanline = 0;
    BootInfo->framebuffer.reserved          = 0;

    Status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
        &GopGuid, NULL, (VOID **)&Gop);
    if (EFI_ERROR(Status) || !Gop || !Gop->Mode || !Gop->Mode->Info) {
        Print(L"[IRIS][LOADER] GOP unavailable\r\n");
        return;
    }
    BootInfo->framebuffer.base              = (uint64_t)Gop->Mode->FrameBufferBase;
    BootInfo->framebuffer.size              = (uint64_t)Gop->Mode->FrameBufferSize;
    BootInfo->framebuffer.width             = (uint32_t)Gop->Mode->Info->HorizontalResolution;
    BootInfo->framebuffer.height            = (uint32_t)Gop->Mode->Info->VerticalResolution;
    BootInfo->framebuffer.pixels_per_scanline = (uint32_t)Gop->Mode->Info->PixelsPerScanLine;
}

static EFI_STATUS load_elf_segments(
    EFI_SYSTEM_TABLE *SystemTable,
    VOID *KernelBuffer,
    UINTN KernelSize
) {
    Elf64_Ehdr *Ehdr = (Elf64_Ehdr *)KernelBuffer;
    Elf64_Phdr *Phdrs;
    UINT16 Index;

    if (KernelSize < sizeof(Elf64_Ehdr)) {
        Print(L"[IRIS][LOADER] ELF too small\r\n");
        return EFI_LOAD_ERROR;
    }
    if (Ehdr->e_ident[EI_MAG0] != ELFMAG0 || Ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        Ehdr->e_ident[EI_MAG2] != ELFMAG2 || Ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        Print(L"[IRIS][LOADER] invalid ELF magic\r\n");
        return EFI_UNSUPPORTED;
    }
    if (Ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        Ehdr->e_ident[EI_DATA]  != ELFDATA2LSB ||
        Ehdr->e_machine         != EM_X86_64) {
        Print(L"[IRIS][LOADER] unsupported ELF format\r\n");
        return EFI_UNSUPPORTED;
    }
    if (Ehdr->e_phoff == 0 ||
        Ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        Ehdr->e_phnum == 0) {
        Print(L"[IRIS][LOADER] invalid ELF phdrs\r\n");
        return EFI_LOAD_ERROR;
    }

    Phdrs = (Elf64_Phdr *)((UINT8 *)KernelBuffer + Ehdr->e_phoff);
    for (Index = 0; Index < Ehdr->e_phnum; ++Index) {
        Elf64_Phdr *Phdr = &Phdrs[Index];
        EFI_PHYSICAL_ADDRESS SegmentBase, AllocBase;
        UINT64 SegmentOffsetInPage;
        UINTN  Pages;
        EFI_STATUS Status;

        if (Phdr->p_type != PT_LOAD) continue;
        if (Phdr->p_offset + Phdr->p_filesz > KernelSize) {
            Print(L"[IRIS][LOADER] segment exceeds file\r\n");
            return EFI_LOAD_ERROR;
        }
        SegmentBase         = (EFI_PHYSICAL_ADDRESS)(Phdr->p_paddr ? Phdr->p_paddr : Phdr->p_vaddr);
        AllocBase           = (EFI_PHYSICAL_ADDRESS)align_down(SegmentBase, IRIS_PAGE_SIZE);
        SegmentOffsetInPage = (UINT64)(SegmentBase - AllocBase);
        Pages               = page_count(SegmentOffsetInPage + Phdr->p_memsz);

        Status = uefi_call_wrapper(SystemTable->BootServices->AllocatePages, 4,
            AllocateAddress, EfiLoaderData, Pages, &AllocBase);
        if (EFI_ERROR(Status)) {
            Print(L"[IRIS][LOADER] AllocatePages failed: %r\r\n", Status);
            return Status;
        }
        mem_copy((VOID *)(UINTN)SegmentBase,
                 (UINT8 *)KernelBuffer + Phdr->p_offset,
                 (UINTN)Phdr->p_filesz);
        if (Phdr->p_memsz > Phdr->p_filesz)
            mem_set((UINT8 *)(UINTN)SegmentBase + Phdr->p_filesz,
                    0, (UINTN)(Phdr->p_memsz - Phdr->p_filesz));
    }
    return EFI_SUCCESS;
}

static UINT64 get_elf_entry(VOID *KernelBuffer) {
    return ((Elf64_Ehdr *)KernelBuffer)->e_entry;
}

static EFI_STATUS get_memory_map_safe(
    EFI_SYSTEM_TABLE      *SystemTable,
    EFI_MEMORY_DESCRIPTOR **OutMap,
    UINTN                  *OutMapSize,
    UINTN                  *OutMapKey,
    UINTN                  *OutDescSize,
    UINT32                 *OutDescVersion
) {
    EFI_STATUS           Status;
    UINTN                MapSize     = 0;
    UINTN                MapKey      = 0;
    UINTN                DescSize    = 0;
    UINT32               DescVersion = 0;
    EFI_PHYSICAL_ADDRESS BufAddr;
    UINTN                BufPages;

    Status = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
        &MapSize, NULL, &MapKey, &DescSize, &DescVersion);
    if (Status != EFI_BUFFER_TOO_SMALL) {
        Print(L"[IRIS][LOADER] GetMemoryMap probe failed: %r\r\n", Status);
        return Status;
    }

    MapSize  += 8 * DescSize;
    BufPages  = page_count(MapSize);

    Status = uefi_call_wrapper(SystemTable->BootServices->AllocatePages, 4,
        AllocateAnyPages, EfiLoaderData, BufPages, &BufAddr);
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] AllocatePages(MemMap) failed: %r\r\n", Status);
        return Status;
    }

    MapSize = BufPages * IRIS_PAGE_SIZE;
    Status  = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
        &MapSize, (EFI_MEMORY_DESCRIPTOR *)(UINTN)BufAddr,
        &MapKey, &DescSize, &DescVersion);
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] GetMemoryMap final failed: %r\r\n", Status);
        uefi_call_wrapper(SystemTable->BootServices->FreePages, 2, BufAddr, BufPages);
        return Status;
    }

    *OutMap         = (EFI_MEMORY_DESCRIPTOR *)(UINTN)BufAddr;
    *OutMapSize     = MapSize;
    *OutMapKey      = MapKey;
    *OutDescSize    = DescSize;
    *OutDescVersion = DescVersion;
    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    VOID      *KernelBuffer = NULL;
    UINTN      KernelSize   = 0;
    UINT64     KernelEntry  = 0;
    static struct iris_boot_info BootInfo;

    InitializeLib(ImageHandle, SystemTable);
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);

    Print(L"====================================\r\n");
    Print(L"        IRIS STAGE 3 LOADER         \r\n");
    Print(L"====================================\r\n");
    Print(L"[IRIS][LOADER] UEFI entry OK\r\n");

    BootInfo.magic            = IRIS_BOOTINFO_MAGIC;
    BootInfo.version          = IRIS_BOOTINFO_VERSION;
    BootInfo.mmap_entry_count = 0;

    Print(L"[IRIS][LOADER] loading ELF kernel...\r\n");
    Status = load_file_into_memory(ImageHandle, SystemTable,
                                   IRIS_KERNEL_PATH, &KernelBuffer, &KernelSize);
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] kernel load failed: %r\r\n", Status);
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 5000000);
        return Status;
    }
    Print(L"[IRIS][LOADER] ELF in memory (%llu bytes)\r\n", (unsigned long long)KernelSize);

    Status = load_elf_segments(SystemTable, KernelBuffer, KernelSize);
    if (EFI_ERROR(Status)) {
        Print(L"[IRIS][LOADER] ELF segment load failed: %r\r\n", Status);
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 5000000);
        return Status;
    }

    KernelEntry = get_elf_entry(KernelBuffer);
    fill_framebuffer_info(SystemTable, &BootInfo);

    {
        EFI_MEMORY_DESCRIPTOR *MemMap      = NULL;
        UINTN                  MapSize     = 0;
        UINTN                  MapKey      = 0;
        UINTN                  DescSize    = 0;
        UINT32                 DescVersion = 0;
        UINTN                  EntryCount, i;

        Status = get_memory_map_safe(SystemTable,
                                     &MemMap, &MapSize, &MapKey,
                                     &DescSize, &DescVersion);
        if (EFI_ERROR(Status)) {
            uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 5000000);
            return Status;
        }

        EntryCount = MapSize / DescSize;
        BootInfo.mmap_entry_count = 0;
        for (i = 0; i < EntryCount && BootInfo.mmap_entry_count < IRIS_MMAP_MAX_ENTRIES; i++) {
            EFI_MEMORY_DESCRIPTOR *Desc =
                (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)MemMap + i * DescSize);
            struct iris_mmap_entry *Entry = &BootInfo.mmap[BootInfo.mmap_entry_count];
            Entry->base     = (uint64_t)Desc->PhysicalStart;
            Entry->length   = (uint64_t)(Desc->NumberOfPages * IRIS_PAGE_SIZE);
            Entry->type     = efi_type_to_iris(Desc->Type);
            Entry->reserved = 0;
            BootInfo.mmap_entry_count++;
        }

        Print(L"[IRIS][LOADER] memory map: %llu entries\r\n",
              (unsigned long long)BootInfo.mmap_entry_count);
        Print(L"[IRIS][LOADER] calling ExitBootServices...\r\n");

        Status = uefi_call_wrapper(SystemTable->BootServices->ExitBootServices, 2,
            ImageHandle, MapKey);
        if (EFI_ERROR(Status)) {
            MapSize = 0;
            uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
                &MapSize, MemMap, &MapKey, &DescSize, &DescVersion);
            Status = uefi_call_wrapper(SystemTable->BootServices->ExitBootServices, 2,
                ImageHandle, MapKey);
            if (EFI_ERROR(Status))
                for (;;) __asm__ volatile ("hlt");
        }
    }

    {
        void (*KernelEntryFn)(struct iris_boot_info *) =
            (void (*)(struct iris_boot_info *))(UINTN)KernelEntry;
        KernelEntryFn(&BootInfo);
    }

    for (;;) __asm__ volatile ("hlt");
    return EFI_SUCCESS;
}
