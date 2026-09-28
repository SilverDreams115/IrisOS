/*
 * link_program.ld — linker script for an IRIS PROGRAM.
 *
 * Identical to `link_service.ld` except for one line, and that line is the
 * whole difference between a program and a service at link time.
 *
 * A service is never told where its own program headers are, because nothing
 * ever asks: the loader parses them out of the FILE and the service itself has
 * no reason to look.  So `link_service.ld` starts the image at 0 and the ELF
 * header and program header table sit at file offsets that no `PT_LOAD`
 * covers — they are in the file and not in the address space.
 *
 * A program must be able to find them.  `AT_PHDR` is a MAPPED address, and a
 * dynamic runtime walks that table to find `PT_DYNAMIC`, `PT_TLS` and its own
 * `PT_GNU_RELRO` — this is the first thing musl's startup does.  Reserving
 * `SIZEOF_HEADERS` puts the headers inside the first loadable segment, which
 * is what every real PIE does and what makes `AT_PHDR` answerable at all.
 *
 * Kept as a separate file rather than changed in place because every service
 * in this tree links with the other one, and a layout change that moves every
 * image's entry point is not a thing to do as a side effect.
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS {
    . = SIZEOF_HEADERS;

    /* ── Executable segment (PF_R|PF_X) ─────────────────────────────── */
    .text : {
        *(.text._start)
        *(.text .text.*)
    }

    . = ALIGN(0x1000);

    /* ── Read-only segment (PF_R) ────────────────────────────────────── */
    .rodata : {
        *(.rodata .rodata.*)
    }

    /* Read-only after relocation: global pointer initialisers */
    .data.rel.ro : {
        *(.data.rel.ro .data.rel.ro.*)
    }

    /* Dynamic section — consumed by elf_apply_relocations via PT_DYNAMIC */
    .dynamic : {
        *(.dynamic)
    }

    /* RELA relocation table for R_X86_64_RELATIVE entries */
    .rela.dyn : {
        *(.rela.dyn)
    }

    . = ALIGN(0x1000);

    /* ── Read-write segment (PF_R|PF_W) ─────────────────────────────── */
    .got : {
        *(.got)
        *(.igot)
        *(.got.plt)
    }

    .data : {
        *(.data .data.*)
    }

    . = ALIGN(0x1000);

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    . = ALIGN(0x1000);

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.comment)
        *(.eh_frame .eh_frame_hdr)
        *(.gnu.version*)
        *(.gnu.hash)
        *(.dynsym)
        *(.dynstr)
        *(.interp)
        *(.plt .plt.*)
    }
}
