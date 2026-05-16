/*
 * link_userboot.ld — flat binary linker script for the userboot stub.
 *
 * OUTPUT_FORMAT(binary) produces a raw byte image with no ELF header.
 * The entry section (.text from entry.S) is placed first so that _start
 * is at byte offset 0, matching the kernel's invariant that the userboot
 * entry point is always at USER_TEXT_BASE + 0.
 *
 * Linked with: ld -nostdlib -static -T link_userboot.ld
 * (no -pie: OUTPUT_FORMAT(binary) is not an ELF format)
 */

OUTPUT_FORMAT(binary)

SECTIONS
{
  . = 0;

  /* entry.S _start must be byte 0 */
  .text : {
    build/ub_entry.o(.text*)
    *(.text*)
  }

  .rodata : {
    *(.rodata*)
  }

  /* No writable .data: userboot has no global state */
  .data : { *(.data*) }
  .bss  : { *(.bss*) *(COMMON) }

  /DISCARD/ : {
    *(.eh_frame*)
    *(.note*)
    *(.comment)
    *(.dynamic)
    *(.dynsym)
    *(.rela.dyn)
    *(.got*)
    *(.plt*)
    *(.interp)
  }
}
