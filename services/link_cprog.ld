/*
 * link_cprog.ld — an ordinary C program, linked against this system's libc.
 *
 * The same layout `link_libc.ld` uses, plus the two things that make it a
 * PROGRAM rather than a library: a `.interp` naming the interpreter, and a
 * `PT_INTERP` pointing at it.  `svc_loader` reads that header and relocates
 * NOTHING, because the interpreter will — and the interpreter here is the C
 * library itself, which is why a C program's first cross-object call
 * (`__libc_start_main`, out of `crt1.o`) is also the first thing step 6 has to
 * resolve.
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS {
    headers PT_PHDR PHDRS ;
    text    PT_LOAD FILEHDR PHDRS ;
    data    PT_LOAD ;
    dynamic PT_DYNAMIC ;
    interp  PT_INTERP ;
}

SECTIONS {
    . = SIZEOF_HEADERS;

    /* KEPT: nothing references it, and without it the linker may drop the one
     * section that says this program has an interpreter at all. */
    .interp : { KEEP(*(.interp)) } :text :interp

    .hash        : { *(.hash) }        :text
    .dynsym      : { *(.dynsym) }      :text
    .dynstr      : { *(.dynstr) }      :text
    .rela.dyn    : { *(.rela.dyn) }    :text
    .rela.plt    : { *(.rela.plt) }    :text
    .plt         : { *(.plt) *(.iplt) } :text

    .text : {
        *(.text._start)
        *(.text .text.*)
    } :text

    .rodata : { *(.rodata .rodata.*) } :text

    . = ALIGN(0x1000);

    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :data
    .dynamic     : { *(.dynamic) }                    :data :dynamic
    .got         : { *(.got) *(.igot) }               :data
    .got.plt     : { *(.got.plt) *(.igot.plt) }       :data
    .data        : { *(.data .data.*) }               :data
    .bss         : { *(.bss .bss.*) *(COMMON) }       :data

    . = ALIGN(0x1000);

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.comment)
        *(.eh_frame .eh_frame_hdr)
        *(.gnu.version*)
        *(.gnu.hash)
    }
}
