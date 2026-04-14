/*
 * link_service.ld — linker script for IRIS userland service ELF binaries.
 *
 * Services are static ELF64 ET_EXEC binaries linked at a fixed virtual address
 * (SERVICE_LOAD_BASE = USER_PRIVATE_BASE + 0x100000 = 0x0000008000100000).
 *
 * Each service runs in its own isolated address space (unique CR3), so all
 * services may share the same load address without conflict.
 *
 * The kernel's elf_vaddr_in_user_private() validation requires:
 *   vaddr >= USER_PRIVATE_BASE (0x0000008000000000) ✓
 *   vaddr + memsz <= USER_STACK_BASE (near 0x10000000000)  ✓ for any sane service
 *
 * Sections:
 *   .text   — executable, read-only
 *   .rodata — read-only data (constants, string literals)
 *   .data   — read-write initialised data
 *   .bss    — zero-initialised data (loader zeroes all pages, so explicit zeroing
 *             is not needed here — the ELF BSS tail is already zero from the
 *             elf_loader_load page allocation path)
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SERVICE_LOAD_BASE = 0x0000008000100000;

SECTIONS {
    . = SERVICE_LOAD_BASE;

    .text : {
        *(.text._start)
        *(.text*)
    }

    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata*)
    }

    . = ALIGN(0x1000);

    .data : {
        *(.data*)
    }

    . = ALIGN(0x1000);

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    . = ALIGN(0x1000);

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.comment)
        *(.eh_frame*)
    }
}
