/*
 * link_libc.ld — the C library, which is also the interpreter.
 *
 * It differs from `link_program.ld` in the one way that matters for step 6: it
 * KEEPS `.dynsym`, `.dynstr` and `.hash`.  A program's undefined symbols are
 * resolved against this object's symbol table at launch, so an image that
 * discarded it would be a C library nothing could call — and the failure would
 * be an unresolved name at spawn rather than anything a reader would connect
 * to a linker script.
 *
 * `.hash` specifically, and not `.gnu.hash`: `DT_HASH`'s second word is the
 * symbol count, which is the ELF format's own statement of how big the table
 * is.  GNU hash has no such field, and a linker that has to infer the size is
 * a linker that can read past the end of a malformed image.  The build passes
 * `--hash-style=sysv` for the same reason.
 *
 * Two loadable segments and no more, because `objreg` holds an object as
 * exactly two frames: one R+X that every consumer shares, one R+W that each
 * consumer copies.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS {
    headers PT_PHDR PHDRS ;
    text    PT_LOAD FILEHDR PHDRS ;
    data    PT_LOAD ;
    dynamic PT_DYNAMIC ;
}

SECTIONS {
    . = SIZEOF_HEADERS;

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
        *(.interp)
    }
}
