/*
 * link_dynprog.ld — linker script for a program that has an INTERPRETER.
 *
 * `link_program.ld` with two additions, and they are the whole of what makes a
 * program dynamic at link time:
 *
 *   - a `.interp` section, holding the interpreter's NAME.  It is a plain
 *     string in the image; nothing about it is a path, and what resolves it is
 *     the spawner asking the object registry — never a search of a directory
 *     this system does not have.
 *   - a `PT_INTERP` program header pointing at it.  That header is what
 *     `svc_loader` reads to decide NOT to relocate this image, because the
 *     interpreter will, and doing it twice corrupts every relative relocation
 *     silently (test_elf_reloc).
 *
 * It is a separate file rather than a conditional in the other one because an
 * empty `PT_INTERP` is worse than no script: `elf_image_has_interp` would be
 * true for every program in the system, and every one of them would be loaded
 * unrelocated and jump into nothing.
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/*
 * The segments are declared explicitly, for one reason: PT_PHDR.
 *
 * An interpreter has to compute where the program it is relocating was loaded,
 * and the only thing on the initial stack that points into the image is
 * `AT_PHDR` — a MAPPED address of the program header table.  Turning that into
 * a load base needs the table's own unbiased vaddr, and the place ELF keeps
 * that is `PT_PHDR`.  Without it an interpreter has to guess, and every guess
 * is an assumption about the layout this very script chooses.
 *
 * `FILEHDR PHDRS` on the text segment is the other half: it puts the ELF
 * header and the table INSIDE the first loadable segment, so `AT_PHDR` is an
 * address in the address space rather than an offset in a file.
 */
PHDRS {
    headers PT_PHDR PHDRS ;
    text    PT_LOAD FILEHDR PHDRS ;
    data    PT_LOAD ;
    dynamic PT_DYNAMIC ;
    interp  PT_INTERP ;
}

SECTIONS {
    . = SIZEOF_HEADERS;

    /* The interpreter's name, first and KEPT: nothing references it, so
     * without KEEP the linker is entitled to drop the one section that makes
     * this a dynamic program. */
    .interp : {
        KEEP(*(.interp))
    } :text :interp

    /* ── Executable segment (PF_R|PF_X) ─────────────────────────────── */
    .text : {
        *(.text._start)
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :text

    . = ALIGN(0x1000);

    /*
     * ── Writable segment (PF_R|PF_W) ────────────────────────────────
     *
     * `.data.rel.ro`, `.dynamic`, `.rela.dyn` and `.got` are HERE and not in
     * the read-only segment above, and for a program that is deliberate: when
     * there is an interpreter it relocates this image IN THE CHILD'S OWN
     * ADDRESS SPACE, so every site a relative relocation patches has to be
     * writable at the moment it runs.  A service's script can afford to put
     * them in the read-only segment because the loader patches them through a
     * window of its own, before the child exists.
     *
     * Read-only-after-relocation is what `PT_GNU_RELRO` is for, and this
     * system cannot express it yet: `mprotect` here is unmap-and-remap of a
     * whole frame, and a frame does not split.  So these pages stay writable,
     * which is stated in docs/contracts/program.md rather than hidden.
     */
    .data.rel.ro : {
        *(.data.rel.ro .data.rel.ro.*)
    } :data

    .dynamic : {
        *(.dynamic)
    } :data :dynamic

    .rela.dyn : {
        *(.rela.dyn)
    } :data

    .got : {
        *(.got)
        *(.igot)
        *(.got.plt)
    } :data

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    . = ALIGN(0x1000);

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.comment)
        *(.eh_frame .eh_frame_hdr)
        *(.gnu.version*)
        *(.gnu.hash)
        *(.dynsym)
        *(.dynstr)
        *(.plt .plt.*)
    }
}
