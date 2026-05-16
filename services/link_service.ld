/*
 * link_service.ld — linker script for IRIS userland service ELF binaries.
 *
 * Services are static PIE (ET_DYN) ELF64 binaries linked at base address 0.
 * The kernel ELF loader applies an RDTSC-seeded ASLR bias at load time and
 * processes R_X86_64_RELATIVE RELA relocations before the task starts.
 *
 * Each service runs in its own isolated address space (unique CR3), so the
 * loader picks an independent bias for every spawn.
 *
 * Sections:
 *   .text        — executable, read-only (PF_R|PF_X)
 *   .rodata      — read-only constants
 *   .data.rel.ro — global pointer initialisers (RO after relocation)
 *   .dynamic     — DT_RELA / DT_RELASZ / DT_RELAENT entries (for PT_DYNAMIC)
 *   .rela.dyn    — R_X86_64_RELATIVE RELA table
 *   .got         — global offset table (static, resolved at load time)
 *   .data        — read-write initialised data
 *   .bss         — zero-initialised data
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS {
    . = 0;

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
